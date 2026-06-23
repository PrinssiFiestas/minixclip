// MIT License
// Copyright (c) 2026 Lauri Lorenzo Fiestas
// https://github.com/PrinssiFiestas/minixclip/blob/main/LICENSE

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <unistd.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

Display* g_display;
Window   g_window; // dummy

Atom CLIPBOARD;      // selection
Atom UTF8_STRING;    // selection type
Atom MINIXCLIPBOARD; // selection contents
Atom TARGETS;        // supported selection targets

const Atom STRING = XA_STRING; // selection type

#define fail(...) exit(!!fprintf(stderr, "minixclip: "__VA_ARGS__))

// atexit() destructor. X11 documents that you should do this before exiting.
void close_display(void)
{
    XCloseDisplay(g_display);
}

// Paste: Request clipboard contents from whoever is the X11 client currently
// owning it.
void clipboard_to_output(void)
{
    atexit(close_display);

    // Make sure that our property doesn't exist yet. Otherwise, clipboard owner
    // might interpret the old contents as arguments to the request.
    XDeleteProperty(g_display, g_window, MINIXCLIPBOARD);

    // Get clipboard contents. This sends request to clipboard owner to copy
    // the contents to our property.
    XConvertSelection(
        g_display, CLIPBOARD, UTF8_STRING, MINIXCLIPBOARD, g_window, CurrentTime);
    XFlush(g_display);

    while (true) { // wait for the clipboard owner to transfer data.
        XEvent event;
        XNextEvent(g_display, &event);
        switch (event.type) {
        case SelectionNotify:
            goto end_wait;
        // other applications often handle other events here too
        }
    }
    end_wait:;

    char* data = NULL;
    Atom type;
    int format; // 8: char, 16: short, 32: long
    unsigned long data_length;

    // Data is now in our property, get 'em.
    XGetWindowProperty(
        g_display, g_window,
        MINIXCLIPBOARD,
        0, LONG_MAX, False,
        AnyPropertyType,
        &type,
        &format,
        &data_length,
        &(unsigned long){0}, // ignore possibility of partial read for brevity.
        (unsigned char**)&data);

    // UTF8_STRING is non-standard, but ubiquitous nowadays and the best default
    // for text. STRING is practically legacy Latin-1 (transmitted data is often
    // UTF-8 anyway), but it is standard, which makes it a reasonable fallback.
    // Checking that format (8 bits for character) matches our expected format
    // is arguably somewhat redundant, if type is UTF8_STRING or STRING, then
    // it should be 8 always.
    if ((type == UTF8_STRING || type == STRING) && format == 8)
        puts(data);
    else
        fail("We only support UTF-8 strings.\n");

    // cleanup
    XFree(data);
    XDeleteProperty(g_display, g_window, MINIXCLIPBOARD);
    exit(EXIT_SUCCESS);
}

// Copy: Claim ownership of clipboard and send data to whoever requests
// clipboard contents until some other X11 client claims clipboard ownership.
void input_to_clipboard(const char data[], size_t data_length)
{
    // Claim clipboard ownership. NOTE: ICCCM compliance would require not using
    // CurrentTime, but it seems to be working fine. xclip does this too and
    // nobody has complained.
    XSetSelectionOwner(g_display, CLIPBOARD, g_window, CurrentTime);
    if (g_window != XGetSelectionOwner(g_display, CLIPBOARD)) {
        XCloseDisplay(g_display);
        fail("Failed to acquire clipboard ownership.");
    }

    // We need to serve SelectionRequest events until another X11 client claims
    // clipboard ownership. Therefore, we must become daemons by forking and
    // killing our parents, so that the shell won't mess with us. This is also
    // more robust than relying on CLIPBOARD_MANAGER that might not even be
    // available. xclip also does this.
    if (fork() != 0) {
        exit(EXIT_SUCCESS);
        // NOTE: X11 server is shared resource between parent and child, so we
        // must not XCloseDisplay() here, the child still needs it.
    }
    atexit(close_display);

    // Unblock umount
    // https://bugs.debian.org/cgi-bin/bugreport.cgi?bug=267412
    chdir("/");

    while (true) { // serve X11 clients requesting clipboard contents.
        XEvent event;
        XNextEvent(g_display, &event);

        switch (event.type) {
        case SelectionRequest:
            #define request event.xselectionrequest
            if (request.target == TARGETS) { // requestor asking which targets we support
                // NOTE: ICCCM compliance would require handling MULTIPLE
                // target, we'll ignore it for brevity.
                Atom targets[] = { TARGETS, UTF8_STRING }; // we support these
                XChangeProperty( // store supported targets in requestors property.
                    g_display, request.requestor, request.property,
                    XA_ATOM, 32,
                    PropModeReplace,
                    (unsigned char*)targets, sizeof targets / sizeof targets[0]);
            } else if (request.target == UTF8_STRING && request.property != None) {
                // requestor asking for the actual data, store it to requestors property.
                XChangeProperty(
                    g_display, request.requestor, request.property,
                    request.target, 8,
                    PropModeReplace,
                    (unsigned char*)data, data_length);
            }

            // Reply with SelectionNotify to tell the requestor that the
            // requested data is ready to be fetched from the provided property.
            XSelectionEvent reply = {
                .type = SelectionNotify,
                // XSendEvent() will set .serial, .send_event, and .display fields.
                .requestor = request.requestor,
                .selection = request.selection,
                .target    = request.target,
                .property  = request.property,
                .time      = request.time,
            };
            XSendEvent(g_display, request.requestor, 0, 0, (XEvent*)&reply);
            XFlush(g_display);
            #undef request
            break;

        case SelectionClear: // somebody else claimed clipboard ownership
            exit(EXIT_SUCCESS);
            break;
        }
    }
}

int main(int argc, char* argv[])
{
    // --------------------------------
    // Handle Inputs

    enum {
        INPUT_TO_CLIPBOARD,
        CLIPBOARD_TO_OUTPUT,
    } mode;

    size_t      data_length = 0;
    static char data[INT32_MAX]; // hopefully large enough :)

    if (isatty(STDIN_FILENO) && argc < 2) // no input
        mode = CLIPBOARD_TO_OUTPUT;
    else { // dump input to data buffer
        mode = INPUT_TO_CLIPBOARD;
        FILE* in = stdin;
        const char* in_name = "standard input";
        if (isatty(STDIN_FILENO)) {
            in_name = argv[1];
            in = fopen(in_name, "r");
            if (in == NULL)
                fail("Could not open %s: %s\n", in_name, strerror(errno));
        }

        data_length = fread(data, sizeof data[0], sizeof data, in);
        if (ferror(in) != 0)
            fail("Could not read %s: %s\n", in_name, strerror(errno));
        if (data_length >= sizeof data - sizeof"")
            fail("Contents of %s too large (max 2 GB).\n", in_name);
        data[data_length] = '\0';
    }

    // --------------------------------
    // X11 Init

    if ((g_display = XOpenDisplay(NULL)) == NULL)
        exit(!!fprintf(stderr, "minixclip: Could not open display.\n"));

    // Dummy window for events. Events we care about are property changes.
    g_window = XCreateSimpleWindow(
        g_display, DefaultRootWindow(g_display), 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(g_display, g_window, PropertyChangeMask);

    CLIPBOARD      = XInternAtom(g_display, "CLIPBOARD",      False);
    UTF8_STRING    = XInternAtom(g_display, "UTF8_STRING",    False);
    MINIXCLIPBOARD = XInternAtom(g_display, "MINIXCLIPBOARD", False); // arbitrary
    TARGETS        = XInternAtom(g_display, "TARGETS",        False);

    // --------------------------------
    // Do the Thing

    switch (mode) {
    case INPUT_TO_CLIPBOARD:
        input_to_clipboard(data, data_length);
    case CLIPBOARD_TO_OUTPUT:
        clipboard_to_output();
    }
}
