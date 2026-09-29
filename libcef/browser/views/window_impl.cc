// Copyright 2016 The Chromium Embedded Framework Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file.

#include "cef/libcef/browser/views/window_impl.h"

#include <memory>

#include "base/i18n/rtl.h"
#include "base/memory/raw_ptr.h"
#include "cef/libcef/browser/browser_event_util.h"
#include "cef/libcef/browser/context.h"
#include "cef/libcef/browser/thread_util.h"
#include "cef/libcef/browser/views/browser_view_impl.h"
#include "cef/libcef/browser/views/display_impl.h"
#include "cef/libcef/browser/views/fill_layout_impl.h"
#include "cef/libcef/browser/views/layout_util.h"
#include "cef/libcef/browser/views/view_util.h"
#include "cef/libcef/browser/views/widget.h"
#include "cef/libcef/browser/views/window_view.h"
#include "components/constrained_window/constrained_window_views.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/base/test/ui_controls.h"
#include "ui/compositor/compositor.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/controls/button/menu_button.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/controls/webview/webview.h"

#if defined(USE_AURA)
#include "ui/aura/window.h"
#endif  // defined(USE_AURA)

#if BUILDFLAG(IS_WIN)
#include "ui/aura/test/ui_controls_aurawin.h"
#include "ui/display/win/screen_win.h"
#elif BUILDFLAG(IS_OZONE)
#include "ui/aura/env.h"
#include "ui/aura/test/event_generator_delegate_aura.h"
#include "ui/aura/window_tree_host_platform.h"
#include "ui/base/hit_test.h"
#include "ui/display/screen.h"
#include "ui/events/test/event_generator.h"
#include "ui/ozone/public/ozone_platform.h"
#include "ui/platform_window/wm/wm_move_resize_handler.h"
#include "ui/views/widget/desktop_aura/desktop_window_tree_host_platform.h"
#endif

namespace {

#if BUILDFLAG(IS_OZONE)
// Returns true if running on the Wayland platform.
bool IsRunningOnWayland() {
  return ui::OzonePlatform::GetPlatformNameForTest() == "wayland";
}
#endif

// Based on chrome/test/base/interactive_ui_tests_main.cc.
// Note: On Ozone/Wayland, ui_controls requires a test compositor with
// WaylandProxy infrastructure. Instead, we use ui::test::EventGenerator
// which dispatches events directly to the window event dispatcher.
void InitializeUITesting() {
  static bool initialized = false;
  if (!initialized) {
#if BUILDFLAG(IS_WIN)
    aura::test::EnableUIControlsAuraWin();
#elif BUILDFLAG(IS_OZONE)
    if (IsRunningOnWayland()) {
      // On Wayland, we use EventGenerator instead of ui_controls.
      ui::test::EventGeneratorDelegate::SetFactoryFunction(
          base::BindRepeating(&aura::test::EventGeneratorDelegateAura::Create));
    } else {
      // On X11, ui_controls works fine.
      ui_controls::EnableUIControls();
    }
#else
    ui_controls::EnableUIControls();
#endif

    initialized = true;
  }
}

#if defined(USE_AURA)

// This class forwards KeyEvents to the CefWindowImpl associated with a widget.
// This allows KeyEvents to be processed after all other targets.
// Events originating from CefBrowserView will instead be delivered via
// CefBrowserViewImpl::HandleKeyboardEvent.
class CefUnhandledKeyEventHandler : public ui::EventHandler {
 public:
  CefUnhandledKeyEventHandler(CefWindowImpl* window_impl, views::Widget* widget)
      : window_impl_(window_impl),
        widget_(widget),
        window_(widget->GetNativeWindow()) {
    DCHECK(window_);
    window_->AddPostTargetHandler(this);
  }

  CefUnhandledKeyEventHandler(const CefUnhandledKeyEventHandler&) = delete;
  CefUnhandledKeyEventHandler& operator=(const CefUnhandledKeyEventHandler&) =
      delete;

  ~CefUnhandledKeyEventHandler() override {
    window_->RemovePostTargetHandler(this);
  }

  // Implementation of ui::EventHandler:
  void OnKeyEvent(ui::KeyEvent* event) override {
    // Give the FocusManager a chance to handle accelerators first.
    // Widget::OnKeyEvent would normally call this after all EventHandlers have
    // had a shot but we don't want to wait.
    if (widget_->GetFocusManager() &&
        !widget_->GetFocusManager()->OnKeyEvent(*event)) {
      event->StopPropagation();
      return;
    }

    CefKeyEvent cef_event;
    if (GetCefKeyEvent(*event, cef_event) &&
        window_impl_->OnKeyEvent(cef_event)) {
      event->StopPropagation();
    }
  }

 private:
  // Members are guaranteed to outlive this object.
  raw_ptr<CefWindowImpl> window_impl_;
  raw_ptr<views::Widget> widget_;

  // |window_| is the event target that is associated with this class.
  raw_ptr<aura::Window> window_;
};

#endif  // defined(USE_AURA)

}  // namespace

// static
CefRefPtr<CefWindow> CefWindow::CreateTopLevelWindow(
    CefRefPtr<CefWindowDelegate> delegate) {
  return CefWindowImpl::Create(delegate, gfx::kNullAcceleratedWidget);
}

// static
CefRefPtr<CefWindowImpl> CefWindowImpl::Create(
    CefRefPtr<CefWindowDelegate> delegate,
    gfx::AcceleratedWidget parent_widget) {
  CEF_REQUIRE_UIT_RETURN(nullptr);
  CefRefPtr<CefWindowImpl> window = new CefWindowImpl(delegate);
  window->Initialize();
  window->CreateWidget(parent_widget);
  return window;
}

void CefWindowImpl::Show() {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_) {
    shown_as_browser_modal_ = false;
    widget_->Show();
  }
}

void CefWindowImpl::ShowAsBrowserModalDialog(
    CefRefPtr<CefBrowserView> browser_view) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_) {
    auto* browser_view_impl =
        static_cast<CefBrowserViewImpl*>(browser_view.get());

    // |browser_view| must belong to the host widget.
    auto* host_widget = cef_window_view()->host_widget();
    CHECK(host_widget &&
          browser_view_impl->root_view()->GetWidget() == host_widget);

    if (auto web_view = browser_view_impl->web_view()) {
      if (auto web_contents = web_view->web_contents()) {
        shown_as_browser_modal_ = true;
        constrained_window::ShowModalDialog(widget_->GetNativeWindow(),
                                            web_contents);

        // NativeWebContentsModalDialogManagerViews::ManageDialog() disables
        // movement. That has no impact on native frames but interferes with
        // draggable regions.
        widget_->set_movement_disabled(false);
      }
    }
  }
}

void CefWindowImpl::Hide() {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_) {
    widget_->Hide();
  }
}

void CefWindowImpl::CenterWindow(const CefSize& size) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_) {
    widget_->CenterWindow(gfx::Size(size.width, size.height));
  }
}

void CefWindowImpl::Close() {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_ && !widget_->IsClosed()) {
    widget_->Close();
  }
}

bool CefWindowImpl::IsClosed() {
  CEF_REQUIRE_UIT_RETURN(false);
  return destroyed_ || (widget_ && widget_->IsClosed());
}

void CefWindowImpl::Activate() {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_ && widget_->CanActivate() && !widget_->IsActive()) {
    widget_->Activate();
  }
}

void CefWindowImpl::Deactivate() {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_ && widget_->CanActivate() && widget_->IsActive()) {
    widget_->Deactivate();
  }
}

bool CefWindowImpl::IsActive() {
  CEF_REQUIRE_VALID_RETURN(false);
  if (widget_) {
    return widget_->IsActive();
  }
  return false;
}

void CefWindowImpl::BringToTop() {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_) {
    widget_->StackAtTop();
  }
}

void CefWindowImpl::SetAlwaysOnTop(bool on_top) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_ && on_top != (widget_->GetZOrderLevel() ==
                            ui::ZOrderLevel::kFloatingWindow)) {
    widget_->SetZOrderLevel(on_top ? ui::ZOrderLevel::kFloatingWindow
                                   : ui::ZOrderLevel::kNormal);
  }
}

bool CefWindowImpl::IsAlwaysOnTop() {
  CEF_REQUIRE_VALID_RETURN(false);
  if (widget_) {
    return widget_->GetZOrderLevel() == ui::ZOrderLevel::kFloatingWindow;
  }
  return false;
}

void CefWindowImpl::Maximize() {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_ && !widget_->IsMaximized()) {
    widget_->Maximize();
  }
}

void CefWindowImpl::Minimize() {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_ && !widget_->IsMinimized()) {
    widget_->Minimize();
  }
}

void CefWindowImpl::Restore() {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_ && (widget_->IsMaximized() || widget_->IsMinimized())) {
    widget_->Restore();
  }
}

void CefWindowImpl::SetFullscreen(bool fullscreen) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_ && fullscreen != widget_->IsFullscreen()) {
    if (CefWidget::GetForWidget(widget_)->ToggleFullscreenMode()) {
      // Received special handling.
      return;
    }

    // Call the Widget method directly with Alloy style, or Chrome style
    // when no BrowserView exists.
    widget_->SetFullscreen(fullscreen);

    // Use a synchronous callback notification on Windows/Linux. Chrome style
    // on Windows/Linux gets notified synchronously via ChromeBrowserDelegate
    // callbacks when a BrowserView exists. MacOS (both runtime styles) gets
    // notified asynchronously via CefNativeWidgetMac callbacks.
#if !BUILDFLAG(IS_MAC)
    if (delegate()) {
      delegate()->OnWindowFullscreenTransition(this, /*is_completed=*/true);
    }
#endif
  }
}

bool CefWindowImpl::IsMaximized() {
  CEF_REQUIRE_VALID_RETURN(false);
  if (widget_) {
    return widget_->IsMaximized();
  }
  return false;
}

bool CefWindowImpl::IsMinimized() {
  CEF_REQUIRE_VALID_RETURN(false);
  if (widget_) {
    return widget_->IsMinimized();
  }
  return false;
}

bool CefWindowImpl::IsFullscreen() {
  CEF_REQUIRE_VALID_RETURN(false);
  if (widget_) {
    return widget_->IsFullscreen();
  }
  return false;
}

CefRefPtr<CefView> CefWindowImpl::GetFocusedView() {
  CEF_REQUIRE_VALID_RETURN(nullptr);
  if (widget_ && widget_->GetFocusManager()) {
    if (auto* focused_view = widget_->GetFocusManager()->GetFocusedView()) {
      return view_util::GetFor(focused_view, /*find_known_parent=*/true);
    }
  }
  return nullptr;
}

void CefWindowImpl::SetTitle(const CefString& title) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (root_view()) {
    root_view()->SetTitle(title);
  }
}

CefString CefWindowImpl::GetTitle() {
  CEF_REQUIRE_VALID_RETURN(CefString());
  if (root_view()) {
    return root_view()->title();
  }
  return CefString();
}

void CefWindowImpl::SetWindowIcon(CefRefPtr<CefImage> image) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (root_view()) {
    root_view()->SetWindowIcon(image);
  }
}

CefRefPtr<CefImage> CefWindowImpl::GetWindowIcon() {
  CEF_REQUIRE_VALID_RETURN(nullptr);
  if (root_view()) {
    return root_view()->window_icon();
  }
  return nullptr;
}

void CefWindowImpl::SetWindowAppIcon(CefRefPtr<CefImage> image) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (root_view()) {
    root_view()->SetWindowAppIcon(image);
  }
}

CefRefPtr<CefImage> CefWindowImpl::GetWindowAppIcon() {
  CEF_REQUIRE_VALID_RETURN(nullptr);
  if (root_view()) {
    return root_view()->window_app_icon();
  }
  return nullptr;
}

CefRefPtr<CefOverlayController> CefWindowImpl::AddOverlayView(
    CefRefPtr<CefView> view,
    cef_docking_mode_t docking_mode,
    bool can_activate) {
  CEF_REQUIRE_VALID_RETURN(nullptr);
  if (root_view()) {
    return root_view()->AddOverlayView(view, docking_mode, can_activate);
  }
  return nullptr;
}

void CefWindowImpl::GetDebugInfo(base::DictValue* info, bool include_children) {
  ParentClass::GetDebugInfo(info, include_children);
  if (root_view()) {
    info->Set("title", root_view()->title());
  }
}

void CefWindowImpl::ShowMenu(CefRefPtr<CefMenuModel> menu_model,
                             const CefPoint& screen_point,
                             cef_menu_anchor_position_t anchor_position) {
  ShowMenu(nullptr, menu_model, screen_point, anchor_position);
}

void CefWindowImpl::Detach() {
  // OnWindowViewDeleted should always be called before Detach().
  DCHECK(!widget_);

  ParentClass::Detach();
}

void CefWindowImpl::SetBounds(const CefRect& bounds) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_) {
    widget_->SetBounds(
        gfx::Rect(bounds.x, bounds.y, bounds.width, bounds.height));
  }
}

CefRect CefWindowImpl::GetBounds() {
  CEF_REQUIRE_VALID_RETURN(CefRect());
  gfx::Rect bounds;
  if (widget_) {
    bounds = widget_->GetWindowBoundsInScreen();
  }
  return CefRect(bounds.x(), bounds.y(), bounds.width(), bounds.height());
}

CefRect CefWindowImpl::GetBoundsInScreen() {
  return GetBounds();
}

void CefWindowImpl::SetSize(const CefSize& size) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_) {
    widget_->SetSize(gfx::Size(size.width, size.height));
  }
}

void CefWindowImpl::SetPosition(const CefPoint& position) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_) {
    gfx::Rect bounds = widget_->GetWindowBoundsInScreen();
    bounds.set_origin(gfx::Point(position.x, position.y));
    widget_->SetBounds(bounds);
  }
}

void CefWindowImpl::SizeToPreferredSize() {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_) {
    if (widget_->non_client_view()) {
      widget_->SetSize(widget_->non_client_view()->GetPreferredSize());
    } else {
      widget_->SetSize(root_view()->GetPreferredSize());
    }
  }
}

void CefWindowImpl::SetVisible(bool visible) {
  if (visible) {
    Show();
  } else {
    Hide();
  }
}

bool CefWindowImpl::IsVisible() {
  CEF_REQUIRE_VALID_RETURN(false);
  if (widget_) {
    return widget_->IsVisible();
  }
  return false;
}

bool CefWindowImpl::IsDrawn() {
  return IsVisible();
}

void CefWindowImpl::SetBackgroundColor(cef_color_t color) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  ParentClass::SetBackgroundColor(color);
  if (widget_ && widget_->GetCompositor()) {
    widget_->GetCompositor()->SetBackgroundColor(color);
  }
}

bool CefWindowImpl::CanWidgetClose() {
  if (shown_as_browser_modal_) {
    // Always allow the close for browser modal dialogs to avoid an infinite
    // loop in WebContentsModalDialogManager::CloseAllDialogs().
    return true;
  }
  if (delegate()) {
    return delegate()->CanClose(this);
  }
  return true;
}

void CefWindowImpl::OnWindowClosing() {
#if defined(USE_AURA)
  unhandled_key_event_handler_.reset();
#endif

  if (delegate()) {
    delegate()->OnWindowClosing(this);
  }
}

void CefWindowImpl::OnWindowViewDeleted() {
  CancelMenu();

  destroyed_ = true;
  widget_ = nullptr;

  if (delegate()) {
    delegate()->OnWindowDestroyed(this);
  }

  // Call Detach() here instead of waiting for the root View to be deleted so
  // that any following attempts to call CefWindow methods from the delegate
  // will fail.
  Detach();
}

// Will only be called if CanHandleAccelerators() returns true.
bool CefWindowImpl::AcceleratorPressed(const ui::Accelerator& accelerator) {
  for (const auto& [key, value] : accelerator_map_) {
    if (value == accelerator) {
      return delegate()->OnAccelerator(this, key);
    }
  }
  return false;
}

bool CefWindowImpl::CanHandleAccelerators() const {
  if (delegate() && widget_ && root_view()) {
    return root_view()->CanHandleAccelerators();
  }
  return false;
}

bool CefWindowImpl::OnKeyEvent(const CefKeyEvent& event) {
  if (delegate()) {
    return delegate()->OnKeyEvent(this, event);
  }
  return false;
}

void CefWindowImpl::ShowMenu(views::MenuButton* menu_button,
                             CefRefPtr<CefMenuModel> menu_model,
                             const CefPoint& screen_point,
                             cef_menu_anchor_position_t anchor_position) {
  CancelMenu();

  if (!widget_) {
    return;
  }

  CefMenuModelImpl* menu_model_impl =
      static_cast<CefMenuModelImpl*>(menu_model.get());
  if (!menu_model_impl || !menu_model_impl->model()) {
    return;
  }

  menu_model_ = menu_model_impl;

  // We'll send the MenuClosed notification manually for better accuracy.
  menu_model_->set_auto_notify_menu_closed(false);

  menu_runner_ = std::make_unique<views::MenuRunner>(
      menu_model_impl->model(),
      menu_button ? views::MenuRunner::HAS_MNEMONICS
                  : views::MenuRunner::CONTEXT_MENU,
      base::BindRepeating(&CefWindowImpl::MenuClosed, this));

  menu_runner_->RunMenuAt(
      widget_, menu_button ? menu_button->button_controller() : nullptr,
      gfx::Rect(gfx::Point(screen_point.x, screen_point.y), gfx::Size()),
      static_cast<views::MenuAnchorPosition>(anchor_position),
      ui::mojom::MenuSourceType::kNone);
}

void CefWindowImpl::MenuClosed() {
  menu_model_->NotifyMenuClosed();
  menu_model_ = nullptr;
  menu_runner_.reset(nullptr);
}

void CefWindowImpl::CancelMenu() {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (menu_runner_) {
    menu_runner_->Cancel();
  }
  DCHECK(!menu_model_);
  DCHECK(!menu_runner_);
}

// Begin a native interactive window move via the platform's WmMoveResizeHandler.
// On Linux/Wayland this dispatches xdg_toplevel.move using the most recent
// input serial; on Linux/X11 it dispatches _NET_WM_MOVERESIZE. Both are
// non-blocking — the compositor takes over until the user releases the
// mouse button.
//
// Intended caller: a renderer-side mousedown handler that forwards the
// event via IPC. This lets clients implement "drag the window from
// anywhere on the title bar" without `-webkit-app-region: drag`, which
// suppresses ALL renderer events on the dragged element (including
// `contextmenu`) and so prevents drag and right-click from coexisting.
bool CefWindowImpl::BeginWindowDrag() {
  CEF_REQUIRE_VALID_RETURN(false);
  if (!widget_) {
    return false;
  }
#if BUILDFLAG(IS_OZONE)
  // Get the underlying ui::PlatformWindow from the Aura tree host. On
  // Linux this is WaylandWindow (Wayland) or X11Window (X11).
  auto* native_view = widget_->GetNativeView();
  if (!native_view) {
    return false;
  }
  auto* host = native_view->GetHost();
  if (!host) {
    return false;
  }
  // On Ozone the Aura tree host is always a WindowTreeHostPlatform
  // subclass (DesktopWindowTreeHostLinux on Wayland/X11). chromium
  // builds with -fno-rtti so we can't dynamic_cast to verify; the
  // BUILDFLAG(IS_OZONE) gate is the static guarantee. If a future
  // Ozone backend ships a non-PlatformWindow tree host, this cast
  // and the platform_window() call below would need to be reworked.
  auto* platform_host = static_cast<aura::WindowTreeHostPlatform*>(host);
  auto* platform_window = platform_host->platform_window();
  if (!platform_window) {
    return false;
  }
  auto* handler = ui::GetWmMoveResizeHandler(*platform_window);
  if (!handler) {
    return false;
  }
  // Get the cursor's current screen position in pixels. WaylandToplevelWindow's
  // HTCAPTION path ignores this (it just calls xdg_toplevel.move with the
  // most recent input serial), but X11Window's path passes it through to
  // _NET_WM_MOVERESIZE which uses it as the drag anchor — passing
  // gfx::Point() there gives a wrong anchor offset. display::Screen returns
  // the same coordinate space the X server uses for root-window events.
  gfx::Point cursor_screen_point;
  if (auto* screen = display::Screen::Get()) {
    cursor_screen_point = screen->GetCursorScreenPoint();
  }
  handler->DispatchHostWindowDragMovement(HTCAPTION, cursor_screen_point);
  return true;
#else
  return false;
#endif
}

CefRefPtr<CefDisplay> CefWindowImpl::GetDisplay() {
  CEF_REQUIRE_VALID_RETURN(nullptr);
  if (widget_ && root_view()) {
    const display::Display& display = root_view()->GetDisplay();
    if (display.is_valid()) {
      return new CefDisplayImpl(display);
    }
  }
  return nullptr;
}

CefRect CefWindowImpl::GetClientAreaBoundsInScreen() {
  CEF_REQUIRE_VALID_RETURN(CefRect());
  if (widget_) {
    gfx::Rect bounds = widget_->GetClientAreaBoundsInScreen();

    if (auto* frame_view = root_view()->GetFrameView()) {
      // When using a custom drawn FrameView the native Window will not
      // know the actual client bounds. Adjust the native Window bounds for the
      // reported client bounds.
      const gfx::Rect& client_bounds = frame_view->GetBoundsForClientView();
      bounds.set_origin(bounds.origin() + client_bounds.OffsetFromOrigin());
      bounds.set_size(client_bounds.size());
    }

    return CefRect(bounds.x(), bounds.y(), bounds.width(), bounds.height());
  }
  return CefRect();
}

void CefWindowImpl::SetDraggableRegions(
    const std::vector<CefDraggableRegion>& regions) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (root_view()) {
    root_view()->SetDraggableRegions(regions);
  }
}

CefWindowHandle CefWindowImpl::GetWindowHandle() {
  CEF_REQUIRE_VALID_RETURN(kNullWindowHandle);
  return view_util::GetWindowHandle(widget_);
}

#if BUILDFLAG(IS_OZONE)
void CefWindowImpl::EnsureEventGenerator(const gfx::Point& screen_point) {
  // Find the window at the given screen coordinates. This handles cases where
  // the target is a popup menu or other window that's not the main window.
  aura::Window* target_root_window = nullptr;
  aura::Window* capture_window = nullptr;
  aura::Window* bounds_window = nullptr;

  aura::Window::Windows windows =
      views::DesktopWindowTreeHostPlatform::GetAllOpenWindows();
  for (aura::Window* window : windows) {
    // Prioritize windows with capture (e.g., popup menus).
    if (window->HasCapture()) {
      capture_window = window->GetRootWindow();
    }
    if (window->GetBoundsInScreen().Contains(screen_point)) {
      bounds_window = window->GetRootWindow();
    }
  }

  // Prefer the window with capture, then the window at the point.
  target_root_window = capture_window ? capture_window : bounds_window;

  // Fall back to this widget's window if no window found.
  if (!target_root_window) {
    gfx::NativeWindow native_window = view_util::GetNativeWindow(widget_);
    if (native_window) {
      target_root_window = native_window->GetRootWindow();
    }
  }

  if (!target_root_window) {
    // Clear any existing EventGenerator to avoid using a stale one.
    event_generator_.reset();
    event_generator_root_window_ = nullptr;
    return;
  }

  // Only recreate EventGenerator if targeting a different root window.
  if (event_generator_ && event_generator_root_window_ == target_root_window) {
    return;
  }

  event_generator_ =
      std::make_unique<ui::test::EventGenerator>(target_root_window);
  event_generator_root_window_ = target_root_window;
}
#endif

void CefWindowImpl::SendKeyPress(int key_code, uint32_t event_flags) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  InitializeUITesting();

  gfx::NativeWindow native_window = view_util::GetNativeWindow(widget_);
  if (!native_window) {
    return;
  }

#if BUILDFLAG(IS_OZONE)
  if (IsRunningOnWayland()) {
    // On Wayland, use EventGenerator which dispatches events directly
    // to the window event dispatcher without requiring test compositor support.
    gfx::Rect bounds = widget_->GetWindowBoundsInScreen();
    EnsureEventGenerator(bounds.CenterPoint());
    if (!event_generator_) {
      return;
    }

    int flags = ui::EF_NONE;
    if (event_flags & EVENTFLAG_CONTROL_DOWN) {
      flags |= ui::EF_CONTROL_DOWN;
    }
    if (event_flags & EVENTFLAG_SHIFT_DOWN) {
      flags |= ui::EF_SHIFT_DOWN;
    }
    if (event_flags & EVENTFLAG_ALT_DOWN) {
      flags |= ui::EF_ALT_DOWN;
    }
    // Use PressAndReleaseKeyAndModifierKeys which properly presses modifier
    // keys before the main key, ensuring accelerators work correctly.
    event_generator_->PressAndReleaseKeyAndModifierKeys(
        static_cast<ui::KeyboardCode>(key_code), flags);
    return;
  }
#endif

  ui_controls::SendKeyPress(native_window,
                            static_cast<ui::KeyboardCode>(key_code),
                            !!(event_flags & EVENTFLAG_CONTROL_DOWN),
                            !!(event_flags & EVENTFLAG_SHIFT_DOWN),
                            !!(event_flags & EVENTFLAG_ALT_DOWN),
                            false);  // Command key is not supported by Aura.
}

void CefWindowImpl::SendMouseMove(int screen_x, int screen_y) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  InitializeUITesting();

  gfx::Point point(screen_x, screen_y);

#if BUILDFLAG(IS_OZONE)
  if (IsRunningOnWayland()) {
    EnsureEventGenerator(point);
    if (event_generator_) {
      event_generator_->MoveMouseTo(point);
    }
    return;
  }
#endif

  // Converts to pixel coordinates internally on Windows.
  ui_controls::SendMouseMove(point.x(), point.y());
}

void CefWindowImpl::SendMouseEvents(cef_mouse_button_type_t button,
                                    bool mouse_down,
                                    bool mouse_up) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (!mouse_down && !mouse_up) {
    return;
  }

  InitializeUITesting();

#if BUILDFLAG(IS_OZONE)
  if (IsRunningOnWayland()) {
    // Use the last known mouse position to find the correct target window.
    gfx::Point mouse_location = aura::Env::GetInstance()->last_mouse_location();
    EnsureEventGenerator(mouse_location);
    if (!event_generator_) {
      return;
    }

    int flag = ui::EF_LEFT_MOUSE_BUTTON;
    if (button == MBT_MIDDLE) {
      flag = ui::EF_MIDDLE_MOUSE_BUTTON;
    } else if (button == MBT_RIGHT) {
      flag = ui::EF_RIGHT_MOUSE_BUTTON;
    }

    if (mouse_down) {
      event_generator_->PressButton(flag);
    }
    if (mouse_up) {
      event_generator_->ReleaseButton(flag);
    }
    return;
  }
#endif

  ui_controls::MouseButton type = ui_controls::LEFT;
  if (button == MBT_MIDDLE) {
    type = ui_controls::MIDDLE;
  } else if (button == MBT_RIGHT) {
    type = ui_controls::RIGHT;
  }

  int state = 0;
  if (mouse_down) {
    state |= ui_controls::DOWN;
  }
  if (mouse_up) {
    state |= ui_controls::UP;
  }

  ui_controls::SendMouseEvents(type, state);
}

void CefWindowImpl::SetAccelerator(int command_id,
                                   int key_code,
                                   bool shift_pressed,
                                   bool ctrl_pressed,
                                   bool alt_pressed,
                                   bool high_priority) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (!widget_) {
    return;
  }

  AcceleratorMap::const_iterator it = accelerator_map_.find(command_id);
  if (it != accelerator_map_.end()) {
    RemoveAccelerator(command_id);
  }

  int modifiers = 0;
  if (shift_pressed) {
    modifiers |= ui::EF_SHIFT_DOWN;
  }
  if (ctrl_pressed) {
    modifiers |= ui::EF_CONTROL_DOWN;
  }
  if (alt_pressed) {
    modifiers |= ui::EF_ALT_DOWN;
  }
  ui::Accelerator accelerator(static_cast<ui::KeyboardCode>(key_code),
                              modifiers);

  accelerator_map_.insert(std::make_pair(command_id, accelerator));

  views::FocusManager* focus_manager = widget_->GetFocusManager();
  DCHECK(focus_manager);
  focus_manager->RegisterAccelerator(
      accelerator,
      high_priority ? ui::AcceleratorManager::kHighPriority
                    : ui::AcceleratorManager::kNormalPriority,
      this);
}

void CefWindowImpl::RemoveAccelerator(int command_id) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (!widget_) {
    return;
  }

  AcceleratorMap::iterator it = accelerator_map_.find(command_id);
  if (it == accelerator_map_.end()) {
    return;
  }

  ui::Accelerator accelerator = it->second;

  accelerator_map_.erase(it);

  views::FocusManager* focus_manager = widget_->GetFocusManager();
  DCHECK(focus_manager);
  focus_manager->UnregisterAccelerator(accelerator, this);
}

void CefWindowImpl::RemoveAllAccelerators() {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (!widget_) {
    return;
  }

  accelerator_map_.clear();

  views::FocusManager* focus_manager = widget_->GetFocusManager();
  DCHECK(focus_manager);
  focus_manager->UnregisterAccelerators(this);
}

void CefWindowImpl::SetThemeColor(int color_id, cef_color_t color) {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (root_view()) {
    view_util::SetColor(root_view(), color_id, color);
  }
}

void CefWindowImpl::ThemeChanged() {
  CEF_REQUIRE_VALID_RETURN_VOID();
  if (widget_) {
    widget_->ThemeChanged();
  }
}

cef_runtime_style_t CefWindowImpl::GetRuntimeStyle() {
  CEF_REQUIRE_VALID_RETURN(CEF_RUNTIME_STYLE_DEFAULT);
  if (auto* window_view = cef_window_view()) {
    return window_view->IsAlloyStyle() ? CEF_RUNTIME_STYLE_ALLOY
                                       : CEF_RUNTIME_STYLE_CHROME;
  }
  return CEF_RUNTIME_STYLE_DEFAULT;
}

CefWindowView* CefWindowImpl::cef_window_view() const {
  return static_cast<CefWindowView*>(root_view());
}

CefWindowImpl::CefWindowImpl(CefRefPtr<CefWindowDelegate> delegate)
    : ParentClass(delegate) {}

CefWindowView* CefWindowImpl::CreateRootView() {
  return new CefWindowView(delegate(), this);
}

void CefWindowImpl::InitializeRootView() {
  cef_window_view()->Initialize();
}

void CefWindowImpl::CreateWidget(gfx::AcceleratedWidget parent_widget) {
  DCHECK(!widget_);

  root_view()->CreateWidget(parent_widget);
  widget_ = root_view()->GetWidget();
  DCHECK(widget_);

#if defined(USE_AURA)
  unhandled_key_event_handler_ =
      std::make_unique<CefUnhandledKeyEventHandler>(this, widget_);
#endif

  // AgentMux/CEF transparency patch: if the global CefSettings background is
  // transparent, push that to the browser-side ui::Compositor now that
  // widget_ has been initialized. Calling earlier (e.g. from
  // OnNativeWidgetCreated, where window_view.cc's existing modal-only path
  // calls SetBackgroundColor) is a no-op because widget_ is still null and
  // widget_->GetCompositor() returns null — that path silently dropped the
  // SetBackgroundColor call, leaving the browser-side compositor at its
  // default opaque white clear color, which then filled the wl_surface
  // framebuffer with opaque white pixels regardless of
  // CefSettings.background_color.
  if (CefContext::Get() &&
      CefContext::Get()->GetBackgroundColor(nullptr, STATE_ENABLED) ==
          SK_ColorTRANSPARENT) {
    SetBackgroundColor(SK_ColorTRANSPARENT);
  }

  // The Widget and root View are owned by the native window. Therefore don't
  // keep an owned reference.
  std::unique_ptr<views::View> view_ptr = view_util::PassOwnership(this);
  [[maybe_unused]] views::View* view = view_ptr.release();

  initialized_ = true;

  if (delegate()) {
    delegate()->OnWindowCreated(this);
  }
}
