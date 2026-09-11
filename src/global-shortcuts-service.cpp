// SPDX-License-Identifier: GPL-3.0-or-later
#include "global-shortcuts-service.h"

#include <algorithm>

#include <gio/gio.h>

namespace gnomos
{

namespace
{
constexpr const char* kPortalBusName = "org.freedesktop.portal.Desktop";
constexpr const char* kPortalObjectPath = "/org/freedesktop/portal/desktop";
constexpr const char* kGlobalShortcutsInterface = "org.freedesktop.portal.GlobalShortcuts";
constexpr const char* kRequestInterface = "org.freedesktop.portal.Request";

// The id is what GnomosWindow's own "win.<id>" action is named — see
// GnomosWindow's "reached two ways" comment on play-pause/next/previous
// for why those specifically exist as real actions. The description is
// what GNOME Settings' "Tastenkombinationen" panel shows the user next to
// whatever key they choose to bind it to — nothing here picks a key.
struct ShortcutSpec
{
  const char* id;
  const char* description;
};
constexpr ShortcutSpec kShortcuts[] = {
    {"play-pause", "Wiedergabe/Pause"},
    {"next", "Nächster Titel"},
    {"previous", "Vorheriger Titel"},
    {"mute-everywhere", "Überall stummschalten"},
};

// The request object path a portal call's Response will eventually arrive
// on is deterministic from the caller's own bus name and a token it
// chooses — see xdg-desktop-portal's own spec on Request objects. Used to
// subscribe *before* making the call rather than after getting the call's
// own (immediate, synchronous-ish) reply back — confirmed live that the
// latter loses a real race: the actual Response can arrive faster than
// this app's own async call_finish()-then-subscribe round trip, silently
// dropping it and leaving the whole chain stuck (BindShortcuts never
// even gets called).
std::string PredictRequestPath(const Glib::RefPtr<Gio::DBus::Connection>& connection, const std::string& token)
{
  std::string sender = connection->get_unique_name();
  if (!sender.empty() && sender.front() == ':')
    sender.erase(sender.begin());
  std::replace(sender.begin(), sender.end(), '.', '_');
  return "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
}

std::string NewToken()
{
  char* guid = g_dbus_generate_guid();
  std::string token = guid;
  g_free(guid);
  return token;
}
}  // namespace

GlobalShortcutsService::GlobalShortcutsService(Gtk::ApplicationWindow& window) : window_(window)
{
  try
  {
    connection_ = Gio::DBus::Connection::get_sync(Gio::DBus::BusType::SESSION);
    proxy_ = Gio::DBus::Proxy::create_sync(connection_, kPortalBusName, kPortalObjectPath, kGlobalShortcutsInterface);
  }
  catch (const Glib::Error&)
  {
    // No session bus, or xdg-desktop-portal (or its GlobalShortcuts
    // implementation specifically) isn't present at all — e.g. a
    // non-GNOME, non-portal-backed compositor. Global shortcuts just
    // never activate; every other control surface (the window itself,
    // MPRIS media keys, the track-change notification's own buttons)
    // keeps working regardless, so this fails quiet rather than loud.
    return;
  }

  // Scoped to the interface, not a specific object path — Activated is
  // emitted on the portal's own single well-known object path for every
  // app with an active session, so OnActivated() itself is what filters
  // by session_handle_ to recognize which of those are actually ours.
  activated_subscription_ = connection_->signal_subscribe(
      [this](const Glib::RefPtr<Gio::DBus::Connection>&, const Glib::ustring&, const Glib::ustring&,
             const Glib::ustring&, const Glib::ustring&, const Glib::VariantContainerBase& parameters) {
        OnActivated(parameters);
      },
      kPortalBusName, kGlobalShortcutsInterface, "Activated", kPortalObjectPath);

  CreateSession();
}

GlobalShortcutsService::~GlobalShortcutsService()
{
  if (!connection_)
    return;
  if (activated_subscription_)
    connection_->signal_unsubscribe(activated_subscription_);
  if (create_session_subscription_)
    connection_->signal_unsubscribe(create_session_subscription_);
  if (bind_shortcuts_subscription_)
    connection_->signal_unsubscribe(bind_shortcuts_subscription_);
}

void GlobalShortcutsService::CreateSession()
{
  std::string handle_token = NewToken();
  // Not actually optional in practice, despite the spec's own wording —
  // confirmed live that omitting session_handle_token doesn't just fail
  // this one call, it crashes the whole xdg-desktop-portal daemon outright
  // (an assertion failure in its own session-object construction, taking
  // down every portal-using app on the desktop until the service
  // auto-restarts).
  std::string session_handle_token = NewToken();

  create_session_subscription_ = connection_->signal_subscribe(
      [this](const Glib::RefPtr<Gio::DBus::Connection>&, const Glib::ustring&, const Glib::ustring&,
             const Glib::ustring&, const Glib::ustring&, const Glib::VariantContainerBase& params) {
        OnCreateSessionResponse(params);
      },
      kPortalBusName, kRequestInterface, "Response", PredictRequestPath(connection_, handle_token));

  GVariantBuilder options_builder;
  g_variant_builder_init(&options_builder, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&options_builder, "{sv}", "handle_token", g_variant_new_string(handle_token.c_str()));
  g_variant_builder_add(&options_builder, "{sv}", "session_handle_token",
                         g_variant_new_string(session_handle_token.c_str()));
  GVariant* parameters = g_variant_new("(@a{sv})", g_variant_builder_end(&options_builder));

  proxy_->call(
      "CreateSession",
      [this](Glib::RefPtr<Gio::AsyncResult>& result) {
        try
        {
          proxy_->call_finish(result);
        }
        catch (const Glib::Error&)
        {
          // The call itself failed outright (as opposed to succeeding and
          // later reporting failure via Response) — nothing will ever
          // arrive on the subscription made above, so it'd otherwise leak
          // for the lifetime of the connection.
          if (create_session_subscription_)
          {
            connection_->signal_unsubscribe(create_session_subscription_);
            create_session_subscription_ = 0;
          }
        }
      },
      Glib::VariantContainerBase(parameters, false));
}

void GlobalShortcutsService::OnCreateSessionResponse(const Glib::VariantContainerBase& parameters)
{
  if (create_session_subscription_)
  {
    connection_->signal_unsubscribe(create_session_subscription_);
    create_session_subscription_ = 0;
  }

  GVariant* response_code_variant = g_variant_get_child_value(const_cast<GVariant*>(parameters.gobj()), 0);
  guint32 response_code = g_variant_get_uint32(response_code_variant);
  g_variant_unref(response_code_variant);
  // Non-zero means the user declined the one-time permission prompt, or
  // the request was cancelled/failed for some other reason — either way,
  // no session to bind shortcuts on.
  if (response_code != 0)
    return;

  GVariant* results = g_variant_get_child_value(const_cast<GVariant*>(parameters.gobj()), 1);
  const char* handle = nullptr;
  if (g_variant_lookup(results, "session_handle", "&s", &handle) && handle)
    session_handle_ = handle;
  g_variant_unref(results);

  if (!session_handle_.empty())
    BindShortcuts();
}

void GlobalShortcutsService::BindShortcuts()
{
  std::string handle_token = NewToken();
  bind_shortcuts_subscription_ = connection_->signal_subscribe(
      [this](const Glib::RefPtr<Gio::DBus::Connection>&, const Glib::ustring&, const Glib::ustring&,
             const Glib::ustring&, const Glib::ustring&, const Glib::VariantContainerBase& params) {
        OnBindShortcutsResponse(params);
      },
      kPortalBusName, kRequestInterface, "Response", PredictRequestPath(connection_, handle_token));

  GVariantBuilder shortcuts_builder;
  g_variant_builder_init(&shortcuts_builder, G_VARIANT_TYPE("a(sa{sv})"));
  for (const ShortcutSpec& shortcut : kShortcuts)
  {
    g_variant_builder_open(&shortcuts_builder, G_VARIANT_TYPE("(sa{sv})"));
    g_variant_builder_add(&shortcuts_builder, "s", shortcut.id);
    g_variant_builder_open(&shortcuts_builder, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&shortcuts_builder, "{sv}", "description", g_variant_new_string(shortcut.description));
    g_variant_builder_close(&shortcuts_builder);
    g_variant_builder_close(&shortcuts_builder);
  }

  GVariantBuilder options_builder;
  g_variant_builder_init(&options_builder, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&options_builder, "{sv}", "handle_token", g_variant_new_string(handle_token.c_str()));

  // No parent_window — this happens once at startup, unprompted by any
  // specific window/dialog of our own, so there's nothing sensible to be
  // transient-for; the system permission dialog just appears unparented.
  GVariant* parameters =
      g_variant_new("(o@a(sa{sv})s@a{sv})", session_handle_.c_str(), g_variant_builder_end(&shortcuts_builder), "",
                    g_variant_builder_end(&options_builder));

  proxy_->call(
      "BindShortcuts",
      [this](Glib::RefPtr<Gio::AsyncResult>& result) {
        try
        {
          proxy_->call_finish(result);
        }
        catch (const Glib::Error&)
        {
          if (bind_shortcuts_subscription_)
          {
            connection_->signal_unsubscribe(bind_shortcuts_subscription_);
            bind_shortcuts_subscription_ = 0;
          }
        }
      },
      Glib::VariantContainerBase(parameters, false));
}

void GlobalShortcutsService::OnBindShortcutsResponse(const Glib::VariantContainerBase&)
{
  // Nothing further to do either way — a non-zero response code here just
  // means the shortcuts stayed unbound (e.g. the user declined), same
  // fail-quiet outcome as everywhere else in this class. Activated
  // deliveries (if any) start arriving on their own from here.
  if (bind_shortcuts_subscription_)
  {
    connection_->signal_unsubscribe(bind_shortcuts_subscription_);
    bind_shortcuts_subscription_ = 0;
  }
}

void GlobalShortcutsService::OnActivated(const Glib::VariantContainerBase& parameters)
{
  GVariant* session_variant = g_variant_get_child_value(const_cast<GVariant*>(parameters.gobj()), 0);
  std::string activated_session = g_variant_get_string(session_variant, nullptr);
  g_variant_unref(session_variant);
  if (activated_session != session_handle_)
    return;

  GVariant* id_variant = g_variant_get_child_value(const_cast<GVariant*>(parameters.gobj()), 1);
  std::string shortcut_id = g_variant_get_string(id_variant, nullptr);
  g_variant_unref(id_variant);

  window_.activate_action("win." + shortcut_id);
}

}  // namespace gnomos
