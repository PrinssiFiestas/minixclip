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
Window   g_window;

Atom CLIPBOARD;      // selection
Atom UTF8_STRING;    // selection type
Atom MINIXCLIPBOARD; // selection contents

#define fail(...) exit(!!fprintf(stderr, "minixclip: "__VA_ARGS__))

// atexit() destructor. X11 documents that you should do this before exiting.
void close_display(void)
{
    XCloseDisplay(g_display);
}

void clipboard_to_output(void)
{
    atexit(close_display);

    // Make sure that our property doesn't exist yet. Otherwise, clipboard owner
    // might interpret the old contents as arguments.
    XDeleteProperty(g_display, g_window, MINIXCLIPBOARD);

    // Get clipboard contents. This sends request to clipboard owner to copy
    // the contents to our property.
    XConvertSelection(
        g_display, CLIPBOARD, UTF8_STRING, MINIXCLIPBOARD, g_window, CurrentTime);
    XFlush(g_display);


    while (true) {
        XEvent event;
        XNextEvent(g_display, &event);
        switch (event.type) {
        case SelectionNotify:
            goto end_wait;
        }
    }
    end_wait:;

    char* data = NULL;
    Atom type;
    int format; // number of bits in character
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

    if (type == UTF8_STRING || (type == XA_STRING && format == 8))
        puts(data);
    else
        fail("We only print UTF-8 strings, sorry.\n");

    // cleanup
    XFree(data);
    XDeleteProperty(g_display, g_window, MINIXCLIPBOARD);
}

void input_to_clipboard(const char data[], size_t data_length)
{
    fail("Not implemented...\n");
}

int main(int argc, char* argv[])
{
    enum {
        INPUT_TO_CLIPBOARD,
        CLIPBOARD_TO_OUTPUT,
    } mode;

    size_t      data_length = 0;
    static char data[INT32_MAX]; // hopefully enough :)

    if (isatty(STDIN_FILENO) && argc < 2) // no input
        mode = CLIPBOARD_TO_OUTPUT;
    else {
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

    switch (mode) {
    case INPUT_TO_CLIPBOARD:
        input_to_clipboard(data, data_length);
    case CLIPBOARD_TO_OUTPUT:
        clipboard_to_output();
    }
}
