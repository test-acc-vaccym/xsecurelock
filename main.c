/*
Copyright 2014 Google Inc. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#define _POSIX_C_SOURCE 200112L

#include <locale.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <signal.h>
#include <time.h>
#ifdef HAVE_SCRNSAVER
#include <X11/extensions/scrnsaver.h>
#endif
#ifdef HAVE_XF86MISC
#include <X11/extensions/xf86misc.h>
#endif
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <unistd.h>

#include "auth_child.h"
#include "saver_child.h"

#define WATCH_CHILDREN_HZ 10

#define ALL_POINTER_EVENTS                                                   \
  (ButtonPressMask | ButtonReleaseMask | EnterWindowMask | LeaveWindowMask | \
   PointerMotionMask | PointerMotionHintMask | Button1MotionMask |           \
   Button2MotionMask | Button3MotionMask | Button4MotionMask |               \
   Button5MotionMask | ButtonMotionMask)

char* auth_executable = NULL;
char* saver_executable = NULL;

enum WatchChildrenState {
  WATCH_CHILDREN_NORMAL,
  WATCH_CHILDREN_SAVER_DISABLED,
  WATCH_CHILDREN_FORCE_AUTH
};

bool WatchChildren(enum WatchChildrenState state, const char* stdinbuf) {
  bool auth_running = WantAuthChild(state == WATCH_CHILDREN_FORCE_AUTH);

  // Note: auth_running is true whenever we WANT to run authentication, or it is
  // already running. It may have recently terminated, which we will notice
  // later.
  if (auth_running) {
    // Make sure the saver is shut down to not interfere with the screen.
    WatchSaverChild(saver_executable, false);

    // Actually start the auth child, or notice termination.
    if (WatchAuthChild(auth_executable, state == WATCH_CHILDREN_FORCE_AUTH,
                       stdinbuf, &auth_running)) {
      // Auth performed successfully. Terminate the other children.
      WatchSaverChild(saver_executable, false);
      // Now terminate the screen lock.
      return true;
    }
  }

  // No auth child is running, either because we don't have any running and
  // didn't start one, or because it just terminated.
  // Show the screen saver.
  if (!auth_running) {
    WatchSaverChild(saver_executable, state != WATCH_CHILDREN_SAVER_DISABLED);
  }

  // Do not terminate the screen lock.
  return false;
}

int IgnoreErrorsHandler(Display* display, XErrorEvent* error) {
  char buf[128];
  XGetErrorText(display, error->error_code, buf, sizeof(buf));
  buf[sizeof(buf) - 1] = 0;
  fprintf(stderr, "Got non-fatal X11 error: %s.\n", buf);
  return 0;
}

void usage(const char* me) {
  printf(
      "Usage:\n"
      "  %s auth_<authname> saver_<savername>\n"
      "\n"
      "Example:\n"
      "  %s auth_pamtester saver_blank\n"
      "\n"
      "This software is licensed under the Apache 2.0 License and links to\n"
      "Xlib. Details are available at the following locations:\n"
      "  " DOCS_PATH
      "/COPYING\n"
      "  " DOCS_PATH
      "/LICENSE-APACHE-2.0\n"
      "  " DOCS_PATH "/LICENSE-XLIB\n",
      me, me);
  // FIXME(rpolzer): license note.
}

int main(int argc, char** argv) {
  setlocale(LC_CTYPE, "");

  if (chdir(HELPER_PATH)) {
    fprintf(stderr, "chdir %s", HELPER_PATH);
    return 1;
  }

  if (argc != 3) {
    usage(argv[0]);
    return 1;
  }

  auth_executable = argv[1];
  if (strncmp(auth_executable, "auth_", 5) || strchr(auth_executable, '.')) {
    usage(argv[0]);
    return 1;
  }
  if (access(auth_executable, X_OK)) {
    usage(argv[0]);
    return 1;
  }

  saver_executable = argv[2];
  if (strncmp(saver_executable, "saver_", 5) || strchr(saver_executable, '.')) {
    usage(argv[0]);
    return 1;
  }
  if (access(saver_executable, X_OK)) {
    usage(argv[0]);
    return 1;
  }

  Display* display = XOpenDisplay(NULL);
  if (display == NULL) {
    fprintf(stderr, "Could not connect to $DISPLAY.\n");
    return 1;
  }

  XIM xim = XOpenIM(display, NULL, NULL, NULL);
  if (xim == NULL) {
    fprintf(stderr, "XOpenIM failed. Assuming Latin-1 encoding.\n");
  }

  XColor Black;
  Black.pixel = BlackPixel(display, DefaultScreen(display));
  XQueryColor(display, DefaultColormap(display, DefaultScreen(display)),
              &Black);

  Window root_window = DefaultRootWindow(display);

  XWindowAttributes rootattrs;
  XGetWindowAttributes(display, root_window, &rootattrs);
  int w = DisplayWidth(display, DefaultScreen(display));
  int h = DisplayHeight(display, DefaultScreen(display));
  XSelectInput(display, root_window, StructureNotifyMask);

  Pixmap bg = XCreatePixmap(display, root_window, 1, 1, 1);

  XSetWindowAttributes coverattrs;
  coverattrs.background_pixel = Black.pixel;
  coverattrs.save_under = 1;
  coverattrs.override_redirect = 1;
  coverattrs.cursor =
      XCreatePixmapCursor(display, bg, bg, &Black, &Black, 0, 0);
  coverattrs.event_mask = KeyPressMask | ButtonPressMask | PointerMotionMask;
  Window saver_window = XCreateWindow(
      display, root_window, 0, 0, w, h, 0, CopyFromParent, InputOutput,
      CopyFromParent,
      CWBackPixel | CWOverrideRedirect | CWSaveUnder | CWEventMask | CWCursor,
      &coverattrs);

  // Now provide the window ID as an environment variable (like XScreenSaver).
  char window_id_str[16];
  size_t window_id_len = snprintf(window_id_str, sizeof(window_id_str), "%lu",
                                  (unsigned long)saver_window);
  if (window_id_len <= 0 || window_id_len >= sizeof(window_id_str)) {
    fprintf(stderr, "Window ID doesn't fit into buffer.\n");
    return 1;
  }
  setenv("XSCREENSAVER_WINDOW", window_id_str, true);

  // Initialize XInput so we can get multibyte key events.
  XIC xic = NULL;
  if (xim != NULL) {
    // As we're OverrideRedirect and grabbing input, we can't use any fancy IMs.
    // Therefore, if we can't get a requirement-less IM, we won't use XIM at
    // all.
    xic = XCreateIC(xim, XNInputStyle, XIMPreeditNone | XIMStatusNone,
                    XNClientWindow, saver_window, XNFocusWindow, saver_window,
                    NULL);
    if (xic == NULL) {
      fprintf(stderr, "XCreateIC failed. Assuming Latin-1 encoding.\n");
    }
  }

  XMapWindow(display, saver_window);

#ifdef HAVE_XF86MISC
  if (XF86MiscSetGrabKeysState(display, False) != MiscExtGrabStateSuccess) {
    fprintf(stderr, "Could not set grab keys state.\n");
    return 1;
  }
#endif

  enum WatchChildrenState saver_state = WATCH_CHILDREN_NORMAL;

#ifdef HAVE_SCRNSAVER
  int scrnsaver_event_base, scrnsaver_error_base;
  if (!XScreenSaverQueryExtension(display, &scrnsaver_event_base,
                                  &scrnsaver_error_base)) {
    scrnsaver_event_base = 0;
    scrnsaver_error_base = 0;
  }
  XScreenSaverSelectInput(display, saver_window, ScreenSaverNotifyMask);
#endif

  int retries;
  for (retries = 10; retries >= 0; --retries) {
    if (!XGrabPointer(display, saver_window, True, ALL_POINTER_EVENTS,
                      GrabModeAsync, GrabModeAsync, None, None, CurrentTime)) {
      break;
    }
    nanosleep(&(const struct timespec){0, 100000000L}, NULL);
  }
  if (retries < 0) {
    fprintf(stderr, "Could not grab pointer.\n");
    return 1;
  }
  for (retries = 10; retries >= 0; --retries) {
    if (!XGrabKeyboard(display, saver_window, True, GrabModeAsync,
                       GrabModeAsync, CurrentTime)) {
      break;
    }
    nanosleep(&(const struct timespec){0, 100000000L}, NULL);
  }
  if (retries < 0) {
    fprintf(stderr, "Could not grab keyboard.\n");
    return 1;
  }

  XSetErrorHandler(IgnoreErrorsHandler);

  // Ignore SIGPIPE, as the auth child is explicitly allowed to close its
  // standard input file descriptor.
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGPIPE, &sa, NULL);

  int x11_fd = ConnectionNumber(display);
  for (;;) {
    fd_set in_fds;
    FD_ZERO(&in_fds);
    FD_SET(x11_fd, &in_fds);
    struct timeval tv;
    tv.tv_usec = 1000000 / WATCH_CHILDREN_HZ;
    tv.tv_sec = 0;
    select(x11_fd + 1, &in_fds, 0, 0, &tv);

    if (WatchChildren(saver_state, NULL)) {
      goto done;
    }

    // If something else shows an OverrideRedirect window, we want to stay on
    // top.
    XRaiseWindow(display, saver_window);
    // If something changed our cursor, change it back.
    XDefineCursor(display, saver_window, coverattrs.cursor);
    // Make sure we don't lose grabs.
    XGrabPointer(display, saver_window, True, ALL_POINTER_EVENTS, GrabModeAsync,
                 GrabModeAsync, None, None, CurrentTime);
    XGrabKeyboard(display, saver_window, True, GrabModeAsync, GrabModeAsync,
                  CurrentTime);

    XEvent ev;
    while (XPending(display) && (XNextEvent(display, &ev), 1)) {
      switch (ev.type) {
        case ConfigureNotify:
          if (ev.xconfigure.window == root_window) {
            // Root window size changed. Adjust the saver_window window too!
            w = ev.xconfigure.width;
            h = ev.xconfigure.height;
            XMoveResizeWindow(display, saver_window, 0, 0, w, h);
          }
          break;
        case MotionNotify:
        case ButtonPress:
          if (WatchChildren(WATCH_CHILDREN_FORCE_AUTH, NULL)) {
            goto done;
          }
          break;
        case KeyPress: {
          char buf[10];
          Status status = XLookupNone;
          int len;
          if (xic) {
            // This uses the current locale.
            len = XmbLookupString(xic, &ev.xkey, buf, sizeof(buf) - 1, NULL,
                                  &status);
          } else {
            // This is always Latin-1. Sorry.
            len = XLookupString(&ev.xkey, buf, sizeof(buf) - 1, NULL, NULL);
          }
          if (status == XLookupChars || status == XLookupBoth) {
            if (len > 0 && (size_t)len < sizeof(buf)) {
              if (len == 1 && buf[0] == '\r') {
                buf[0] = '\n';
              }
              buf[len] = 0;
            } else {
              fprintf(stderr,
                      "Received invalid length from XLookupString: %d\n", len);
              buf[0] = 0;
            }
          } else {
            // No new bytes. Fine.
            buf[0] = 0;
          }
          if (WatchChildren(WATCH_CHILDREN_FORCE_AUTH, buf)) {
            goto done;
          }
        } break;
        case KeyRelease:
        case ButtonRelease:
        case MappingNotify:
        case EnterNotify:
        case LeaveNotify:
          // Ignored.
          break;
        default:
#ifdef HAVE_SCRNSAVER
          if (ev.type == scrnsaver_event_base + ScreenSaverNotify) {
            if (((XScreenSaverNotifyEvent*)&ev)->state == ScreenSaverOn) {
              saver_state = WATCH_CHILDREN_SAVER_DISABLED;
            } else {
              saver_state = WATCH_CHILDREN_NORMAL;
            }
            break;
          }
#endif
          break;
      }
    }
  }

done:
  XDestroyWindow(display, saver_window);
  XFreeCursor(display, coverattrs.cursor);
  XFreePixmap(display, bg);

  return EXIT_SUCCESS;
}