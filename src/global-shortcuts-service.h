// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>

#include <giomm/dbusconnection.h>
#include <giomm/dbusproxy.h>
#include <gtkmm/applicationwindow.h>

namespace gnomos
{

// Binds a handful of global keyboard shortcuts (Play/Pause, Next, Previous,
// "Überall stummschalten") via the xdg-desktop-portal GlobalShortcuts
// portal (org.freedesktop.portal.GlobalShortcuts), so they fire even while
// the window is hidden (see run_in_background_'s own comment on
// GnomosWindow) or simply unfocused. Unlike MPRIS's own Play/Pause/Next/
// Previous (MprisService), which only ever react to the desktop's fixed,
// built-in media keys, this lets the user assign *their own* keybinding
// via GNOME Settings' "Tastenkombinationen" panel, under whatever label
// each shortcut's "description" here provides — nothing is bound to a
// specific key by Gnomos itself.
//
// Every portal method that returns an object path here is a *request*:
// the actual outcome arrives later as a Response signal on that same
// path, per the standard xdg-desktop-portal pattern (the same one
// Gtk::FileDialog's own portal backend uses internally) — never a
// synchronous return value, and never guaranteed to succeed at all. The
// very first launch shows the user a one-time system permission dialog;
// declining it (or the portal being unavailable entirely, e.g. a
// non-GNOME/non-portal-backed compositor) just means these shortcuts
// silently never fire — every other way to control playback keeps
// working regardless, so this fails quiet rather than loud.
class GlobalShortcutsService
{
public:
  explicit GlobalShortcutsService(Gtk::ApplicationWindow& window);
  ~GlobalShortcutsService();

  GlobalShortcutsService(const GlobalShortcutsService&) = delete;
  GlobalShortcutsService& operator=(const GlobalShortcutsService&) = delete;

private:
  void CreateSession();
  void OnCreateSessionResponse(const Glib::VariantContainerBase& parameters);
  void BindShortcuts();
  void OnBindShortcutsResponse(const Glib::VariantContainerBase& parameters);
  void OnActivated(const Glib::VariantContainerBase& parameters);

  Gtk::ApplicationWindow& window_;
  Glib::RefPtr<Gio::DBus::Connection> connection_;
  Glib::RefPtr<Gio::DBus::Proxy> proxy_;
  std::string session_handle_;

  // Each request's Response is only ever relevant once — unsubscribed as
  // soon as it fires, rather than left to match (and be silently ignored
  // by an empty session_handle_ check) on some later, unrelated request.
  guint create_session_subscription_ = 0;
  guint bind_shortcuts_subscription_ = 0;
  guint activated_subscription_ = 0;
};

}  // namespace gnomos
