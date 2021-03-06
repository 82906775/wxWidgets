/////////////////////////////////////////////////////////////////////////////
// Name:        src/unix/uiactionx11.cpp
// Purpose:     wxUIActionSimulator implementation
// Author:      Kevin Ollivier, Steven Lamerton, Vadim Zeitlin
// Created:     2010-03-06
// Copyright:   (c) 2010 Kevin Ollivier
//              (c) 2010 Steven Lamerton
//              (c) 2010-2016 Vadim Zeitlin
// Licence:     wxWindows licence
/////////////////////////////////////////////////////////////////////////////

#include "wx/defs.h"

#if wxUSE_UIACTIONSIMULATOR

#include "wx/uiaction.h"
#include "wx/event.h"
#include "wx/evtloop.h"

#include "wx/private/uiaction.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#if wxUSE_XTEST
#include <X11/extensions/XTest.h>
#endif

#include "wx/unix/utilsx11.h"

#ifdef __WXGTK__
#include "wx/window.h"
#include "wx/gtk/private/wrapgtk.h"
#include <gdk/gdkx.h>

GtkWidget* wxGetTopLevelGTK();
GdkWindow* wxGetTopLevelGDK();

// This helper function tries to set the input focus to the correct (top level)
// window, i.e.: the window to which keyboard events will be reported.
static inline void wxSetInputFocusToXWindow(wxX11Display& display)
{
    wxWindow* const win = wxGetActiveWindow();

    GdkWindow* gdkwin;

    if ( win && win->IsTopLevel() )
        gdkwin = gtk_widget_get_window(win->GetHandle());
    else
        gdkwin = wxGetTopLevelGDK();

    XSetInputFocus(display, GDK_WINDOW_XID(gdkwin), RevertToPointerRoot, CurrentTime);
}
#else // !__WXGTK__
#define wxSetInputFocusToXWindow(display)
#endif // __WXGTK__

// Normally we fall back on "plain X" implementation if XTest is not available,
// but it's useless to do it when using GTK+ 3 as it's not going to work with
// it anyhow because GTK+ 3 needs XInput2 events and not the "classic" ones we
// synthesize here, so don't even compile in this code for wxGTK3 port.
#ifdef __WXGTK3__
    #define wxUSE_PLAINX_IMPL 0
#else
    #define wxUSE_PLAINX_IMPL 1
#endif

namespace
{
// Like the real events, this class tries to add a _fake_ delay to the generated
// (fake) events so that chances that they got lost or ignored (for whatever reasons)
// by the X server (or the WM, or even the input device driver) are minimized.
class wxXSync
{
public:
    wxXSync(wxX11Display& display)
        : m_display(display), m_isMotion(true)
    {
    }

    wxXSync(wxX11Display& display, bool depressed)
        : m_display(display), m_isMotion(false)
    {
        depressed ? ++ms_numDepressed : --ms_numDepressed;

        wxASSERT_MSG( ms_numDepressed >= 0, "Invalid call to wxXSync() ctor" );
    }

    ~wxXSync()
    {
        XSync(m_display, False);

        if ( m_isMotion )
        {
            wxYield();
        }
        else // it's button or key event
        {
            if ( ms_numDepressed > 0 )
            {
                // Do nothing if a key / button is still depressed.
                return;
            }

            wxYield();
            wxMilliSleep(Default_Delay);
        }
    }

private:
    wxX11Display& m_display;
    const bool    m_isMotion; // false if it's button or key event.

    enum
    {
        Default_Delay = 20  // amount of ms to sleep after key/button release.
    };

    static int ms_numDepressed;

    wxDECLARE_NO_COPY_CLASS(wxXSync);
};

/*static*/
int wxXSync::ms_numDepressed = 0;

// Base class for both available X11 implementations.
class wxUIActionSimulatorX11Impl : public wxUIActionSimulatorImpl
{
public:
    // Return the most appopriate implementation to use: if XTest is available,
    // use it, otherwise use plain X11 calls.
    //
    // The returned pointer is owned by the caller.
    static wxUIActionSimulatorImpl* New();

    virtual bool MouseMove(long x, long y) wxOVERRIDE;
    virtual bool MouseDown(int button = wxMOUSE_BTN_LEFT) wxOVERRIDE;
    virtual bool MouseUp(int button = wxMOUSE_BTN_LEFT) wxOVERRIDE;

    virtual bool DoKey(int keycode, int modifiers, bool isDown) wxOVERRIDE;

protected:
    // This ctor takes ownership of the display.
    explicit wxUIActionSimulatorX11Impl(wxX11Display& display)
        : m_display(display)
    {
        wxYield();
        wxMilliSleep(50);

        wxSetInputFocusToXWindow(m_display);
    }

    wxX11Display m_display;

private:
    // Common implementation of Mouse{Down,Up}() which just forwards to
    // DoX11Button() after translating wx button to X button constant.
    bool SendButtonEvent(int button, bool isDown);

    virtual bool DoX11Button(int xbutton, bool isDown) = 0;
    virtual bool DoX11MouseMove(long x, long y) = 0;
    virtual bool DoX11Key(KeyCode xkeycode, int modifiers, bool isDown) = 0;

    wxDECLARE_NO_COPY_CLASS(wxUIActionSimulatorX11Impl);
};

bool wxUIActionSimulatorX11Impl::SendButtonEvent(int button, bool isDown)
{
    if ( !m_display )
        return false;

    int xbutton;
    switch (button)
    {
        case wxMOUSE_BTN_LEFT:
            xbutton = 1;
            break;
        case wxMOUSE_BTN_MIDDLE:
            xbutton = 2;
            break;
        case wxMOUSE_BTN_RIGHT:
            xbutton = 3;
            break;
        default:
            wxFAIL_MSG("Unsupported button passed in.");
            return false;
    }

    return DoX11Button(xbutton, isDown);
}

#if wxUSE_PLAINX_IMPL

// Implementation using just plain X11 calls.
class wxUIActionSimulatorPlainX11Impl : public wxUIActionSimulatorX11Impl
{
public:
    explicit wxUIActionSimulatorPlainX11Impl(wxX11Display& display)
        : wxUIActionSimulatorX11Impl(display)
    {
    }

private:
    virtual bool DoX11Button(int xbutton, bool isDown) wxOVERRIDE;
    virtual bool DoX11MouseMove(long x, long y) wxOVERRIDE;
    virtual bool DoX11Key(KeyCode xkeycode, int modifiers, bool isDown) wxOVERRIDE;

    wxDECLARE_NO_COPY_CLASS(wxUIActionSimulatorPlainX11Impl);
};

bool wxUIActionSimulatorPlainX11Impl::DoX11Button(int xbutton, bool isDown)
{
    XEvent event;
    memset(&event, 0x00, sizeof(event));

    event.type = isDown ? ButtonPress : ButtonRelease;
    event.xbutton.button = xbutton;
    event.xbutton.same_screen = True;

    XQueryPointer(m_display, m_display.DefaultRoot(),
                  &event.xbutton.root, &event.xbutton.window,
                  &event.xbutton.x_root, &event.xbutton.y_root,
                  &event.xbutton.x, &event.xbutton.y, &event.xbutton.state);
    event.xbutton.subwindow = event.xbutton.window;

    while (event.xbutton.subwindow)
    {
        event.xbutton.window = event.xbutton.subwindow;
        XQueryPointer(m_display, event.xbutton.window,
                      &event.xbutton.root, &event.xbutton.subwindow,
                      &event.xbutton.x_root, &event.xbutton.y_root,
                      &event.xbutton.x, &event.xbutton.y, &event.xbutton.state);
    }

    wxXSync sync(m_display, isDown);
    XSendEvent(m_display, PointerWindow, True, 0xfff, &event);

    return true;
}

bool wxUIActionSimulatorPlainX11Impl::DoX11MouseMove(long x, long y)
{
    wxXSync sync(m_display);
    Window root = m_display.DefaultRoot();
    XWarpPointer(m_display, None, root, 0, 0, 0, 0, x, y);
    return true;
}

bool
wxUIActionSimulatorPlainX11Impl::DoX11Key(KeyCode xkeycode,
                                          int modifiers,
                                          bool isDown)
{
    int mask, type;

    if ( isDown )
    {
        type = KeyPress;
        mask = KeyPressMask;
    }
    else
    {
        type = KeyRelease;
        mask = KeyReleaseMask;
    }

    Window focus;
    int revert;
    XGetInputFocus(m_display, &focus, &revert);
    if (focus == None)
        return false;

    int mod = 0;

    if (modifiers & wxMOD_SHIFT)
        mod |= ShiftMask;
    //Mod1 is alt in the vast majority of cases
    if (modifiers & wxMOD_ALT)
        mod |= Mod1Mask;
    if (modifiers & wxMOD_CMD)
        mod |= ControlMask;

    XKeyEvent event;
    event.display = m_display;
    event.window = focus;
    event.root = DefaultRootWindow(event.display);
    event.subwindow = None;
    event.time = CurrentTime;
    event.x = 1;
    event.y = 1;
    event.x_root = 1;
    event.y_root = 1;
    event.same_screen = True;
    event.type = type;
    event.state = mod;
    event.keycode = xkeycode;

    wxXSync sync(m_display, isDown);
    XSendEvent(event.display, event.window, True, mask, (XEvent*) &event);

    return true;
}

#endif // wxUSE_PLAINX_IMPL

#if wxUSE_XTEST

// Implementation using XTest extension.
class wxUIActionSimulatorXTestImpl : public wxUIActionSimulatorX11Impl
{
public:
    explicit wxUIActionSimulatorXTestImpl(wxX11Display& display)
        : wxUIActionSimulatorX11Impl(display)
    {
    }

private:
    virtual bool DoX11Button(int xbutton, bool isDown) wxOVERRIDE;
    virtual bool DoX11MouseMove(long x, long y) wxOVERRIDE;
    virtual bool DoX11Key(KeyCode xkeycode, int modifiers, bool isDown) wxOVERRIDE;

    wxDECLARE_NO_COPY_CLASS(wxUIActionSimulatorXTestImpl);
};

bool wxUIActionSimulatorXTestImpl::DoX11Button(int xbutton, bool isDown)
{
    wxXSync sync(m_display, isDown);
    return XTestFakeButtonEvent(m_display, xbutton, isDown, CurrentTime) != 0;
}

bool wxUIActionSimulatorXTestImpl::DoX11MouseMove(long x, long y)
{
#ifdef __WXGTK3__
    // We need to take into account the scaling factor as the input coordinates
    // are in GTK logical "application pixels", while we need the physical
    // "device pixels" for the X call below, so scale them if we have the
    // required support at both compile- and run-time.
#if GTK_CHECK_VERSION(3,10,0)
    if ( gtk_check_version(3, 10, 0) == NULL )
    {
        // For multi-monitor support we would need to determine to which
        // monitor the point (x, y) belongs, for now just use the scale
        // factor of the main one.
        gint const scale = gtk_widget_get_scale_factor(wxGetTopLevelGTK());
        x *= scale;
        y *= scale;
    }
#endif // GTK+ 3.10+
#endif // __WXGTK3__

    wxXSync sync(m_display);
    return XTestFakeMotionEvent(m_display, -1, x, y, CurrentTime) != 0;
}

bool
wxUIActionSimulatorXTestImpl::DoX11Key(KeyCode xkeycode,
                                       int WXUNUSED(modifiers),
                                       bool isDown)
{
    wxXSync sync(m_display, isDown);
    return XTestFakeKeyEvent(m_display, xkeycode, isDown, CurrentTime) != 0;
}

#endif // wxUSE_XTEST

wxUIActionSimulatorImpl* wxUIActionSimulatorX11Impl::New()
{
    wxX11Display display;

#if wxUSE_XTEST
    // If we can fall back on plain X implementation, check if XTest extension
    // is available and if it isn't, use the other one. OTOH if we don't have
    // the other one anyhow, then testing for XTest availability is useless.
#if wxUSE_PLAINX_IMPL
    int dummy;
    if ( XTestQueryExtension(display, &dummy, &dummy, &dummy, &dummy) )
#endif // wxUSE_PLAINX_IMPL
        return new wxUIActionSimulatorXTestImpl(display);
#endif // wxUSE_XTEST

#if wxUSE_PLAINX_IMPL
    return new wxUIActionSimulatorPlainX11Impl(display);
#endif // wxUSE_PLAINX_IMPL
}

} // anonymous namespace

bool wxUIActionSimulatorX11Impl::MouseDown(int button)
{
    return SendButtonEvent(button, true);
}

bool wxUIActionSimulatorX11Impl::MouseMove(long x, long y)
{
    if ( !m_display )
        return false;

#ifdef  __WXGTK20__
    GdkWindow* const gdkwin1 = gdk_window_at_pointer(NULL, NULL);
    const bool ret = DoX11MouseMove(x, y);
    GdkWindow* const gdkwin2 = gdk_window_at_pointer(NULL, NULL);

    if ( gdkwin1 != gdkwin2 )
    {
        // Workaround the problem of destination window not getting a motion event
        // after our fake mouse movement (unless the pointer is already inside it)
        // by issuing a second call to 'DoX11MouseMove()' to get the event.
        // Notice that ret is true here and we don't want to override it by whatever
        // this second call returns.
        DoX11MouseMove(x, y);
    }

    return ret;
#endif

    return DoX11MouseMove(x, y);
}

bool wxUIActionSimulatorX11Impl::MouseUp(int button)
{
    return SendButtonEvent(button, false);
}

bool wxUIActionSimulatorX11Impl::DoKey(int keycode, int modifiers, bool isDown)
{
    if ( !m_display )
        return false;

    WXKeySym xkeysym = wxCharCodeWXToX(keycode);
    KeyCode xkeycode = XKeysymToKeycode(m_display, xkeysym);
    if ( xkeycode == NoSymbol )
        return false;

    return DoX11Key(xkeycode, modifiers, isDown);
}

wxUIActionSimulator::wxUIActionSimulator()
                   : m_impl(wxUIActionSimulatorX11Impl::New())
{
}

wxUIActionSimulator::~wxUIActionSimulator()
{
    delete m_impl;
}

#endif // wxUSE_UIACTIONSIMULATOR
