# minixclip

`minixclip` is a small clone of [xclip](https://github.com/astrand/xclip) command line utility, which can copy data from standard input or a given argument file to X11 clipboard, or paste X11 clipboard contents to standard output. This is not meant to be fully featured, it is just a simple example of how to work with X11 clipboards for anyone currently learning about the subject. 

Supported features:

- Copy UTF-8 strings to CLIPBOARD selection,
- Paste UTF-8 strings from CLIPBOARD selection.

Unsupported features:

- Other selections than CLIPBOARD.
- Non UTF-8 data.
- Large data transfers: INCR property will be ignored.
- Any `xclip` command line options.

## Building

```bash
make
```

## Usage

```bash
minixclip [FILE]
```

If data is given trough standard input, then it will be copied to clipboard. Otherwise, if file argument is passed, then it's contents will be copied to clipboard, otherwise pastes clipboard contents to standard output. 

## Basic Overview of X11 Clipboard

X11 doesn't have a single clipboard, it can have arbitrary number of clipboards called *selections*. We will be referring to clipboards mostly as selections from now on. The three standardized selections are 

- PRIMARY: Most often used when user selects text and pastes using middle click without pressing Ctrl+C. 
- SECONDARY: Obsolete nowadays. Should not be used, no need to implement either. 
- CLIPBOARD: Most often used when user explicitly copies data using Ctrl+C for example. 

Any other selections can be defined by application developers to be used for their own purposes. 

X11 does not store any contents of any selections in the server, the data is stored by X11 clients instead and transferred on demand. Example of a copy-paste interaction might go like this: 

- User selects text in Application 1. 
- Application 1 detects that text has been selected and becomes the owner of the PRIMARY selection. No text has been transferred yet. 
- User pastes text using middle click in Application 2. 
- Application 2 detects that user tries to paste. Application 2 asks X11 server to tell whoever owns the PRIMARY selection (which is Application 1) to send the data. Application 2 does not need to know who owns the selection, but it has to provide a slot (*property*) where the data will be stored. 
- The server tells Application 1 that a window wants the selection data. Application 1 writes the data to a given property of the given window. 
- Application 2 reads the data from the property and finally executes whatever pasting means for that given application. 

## Atoms and Properties

Atoms are simply just integers representing a string. These can be thought of keys to hash maps. Converting a string to an atom is called *string interning*. The idea is that moving integers around and comparing them is much more efficient than dealing with strings. Atoms themselves do not contain data, but can be used as keys to obtain data. Converting strings to atoms can be done using `XInternAtom()`. 

Window properties are key-value pairs, where keys are atoms that describe the name of the property. Properties also have *types* that describes the data defined by atoms. There can be arbitrary number of properties and types defined by the user, but some are built-in. 

Examples of available types: 

| Type Atom     | Format | Description       |
|---------------|--------|-------------------|
| STRING        |      8 | Latin-1 strings   |
| UTF8_STRING   |      8 | UTF-8 strings     |
| COMPOUND_TEXT |      8 | Variable encoding |
| ATOM          |     32 | Atoms             |
| PIXEL         |     32 | Pixels            |

Format describes element size, where 8 means `char`, 16 means `short`, and 32 means `long`. 

Examples of available properties: 

| Name Atom | Type Atom     | Description         |
|-----------|---------------|---------------------|
| PRIMARY   | *various*     | Primary selection   |
| CLIPBOARD | *various*     | Clipboard selection |
| WM_NAME   | COMPOUND_TEXT | Window title name   |

Properties can be managed with `XGetWindowProperty()`, `XListProperties()`, `XChangeProperty()`, `XRotateWindowProperties()`, and `XDeleteProperty()`.

## `minixclip` Implementation

The following code examples are pseudocode that demonstrate how X11 clipboard might be implemented for a `xclip`-like utility. It will be close to the actual implementation found in `minixclip.c`. Some function arguments and other code will be omitted, so we can put our full focus on underlying concepts instead of implementation details. 

The implementation consists of two major parts: copying (`input_to_clipboard()`) and pasting (`clipboard_to_output()`). Roughly the outline of the operations can be described in the following steps: 

Copying: 

1. Claim ownership of selection. This makes us a conceptually a "clipboard server". 
2. Wait for `SelectionRequest` events. 
3. Serve `SelectionRequest` event by copying the data to the window property given in the request. 
4. Send the requestor a `SelectionNotify` event that tells the requestor that the data has been written to it's property. 
5. If another client claimed ownership of our selection, then we receive a `SelectionClear` event and can stop serving. Otherwise, go to step 2. 

Pasting: 

1. Send a `ConvertSelection()` request to X11 server with a property that will be used for the reply. This will trigger `SelectionRequest` event to the owner of the selection. 
2. Wait for `SelectionNotify` event, so that we know when to fetch the data. 
3. Fetch the data from the property we specified in `ConvertSelection()` request. 

### Common Boilerplate

We must open a display and a window to receive events. We'll also intern strings of atoms that we want to use throughout the program. After interning, the atoms will never change. 

```c
Display* g_display;
Window   g_window; // need a window for events

Atom CLIPBOARD;      // selection
Atom UTF8_STRING     // selection type
Atom MINIXCLIPBOARD; // selection contents (arbitrary)
Atom TARGETS;        // supported selection targets
Atom STRING = XA_STRING; // using predefined selection type from <X11/Xatom.h>

int main(int argc, char* argv[])
{
    static char data[];
    if (/* reading input */)
        fread(data, input);

    g_display = XOpenDisplay();
    g_window = XCreateSimpleWindow(g_display); // window with zero size
    XSelectInput(g_window); // select our window to receive events

    CLIPBOARD      = XInternAtom(g_display, "CLIPBOARD");
    UTF8_STRING    = XInternAtom(g_display, "UTF8_STRING");
    MINIXCLIPBOARD = XInternAtom(g_display, "MINIXCLIPBOARD"); // arbitrary
    TARGETS        = XInternAtom(g_display, "TARGETS");

    if (/* reading input */)
        input_to_clipboard(data); // copy
    else
        clipboard_to_output(); // paste
}
```

### Paste

To obtain the clipboard contents, we must request them from the X11 server. We do this by calling `XConvertSelection()`. As arguments we pass which selection are we interested in (CLIPBOARD in our case), what datatype are we expecting (UTF8_STRING in our case), and which property should be used to store the data. The property should be some arbitrary property defined by us that *does not exist* when calling `XConvertSelection()`. Therefore, we should remove it using `XDeleteProperty()`. The reason why we are doing this is that some some selections take arguments, which are passed in that specified property. If we were to not delete the property, then any existing data in it might be misinterpreted as arguments by the selection owner. 

```c
    XDeleteProperty(MINIXCLIPBOARD);
    XConvertSelection(CLIPBOARD, UTF8_STRING, MINIXCLIPBOARD, g_window);
```

`XConvertSelection()` sends selection request event to the owner of the selection. The owner stores the data to the property we passed (MINIXCLIPBOARD) and sends `SelectionNotify` event to our window (`g_window`) once it's ready to be fetched. Let's wait for that event. 

```c
    while (true) {
        XEvent event;
        XNextEvent(&event);
        switch (event.type) {
        case SelectionNotify:
            goto end_wait;
        // often you might handle other events here too
        }
    } end_wait:
```

Now that the contents are ready to be fetched from the property we specified in `XConvertSelection()` (which is MINIXCLIPBOARD), we can read it and finally write the contents to the output. Let's not forget to clean resources! 

```c
    char* data;
    Atom type;
    XGetWindowProperty(MINIXCLIPBOARD, &type, &data);
    if (type == UTF8_STRING) // paste
        puts(data);
    else
        fail();
        
    // cleanup
    XFree(data);
    XDeleteProperty(MINIXCLIPBOARD);
```

Full pseudocode for pasting:

```c
void clipboard_to_output()
{
    // Request selection.
    XDeleteProperty(MINIXCLIPBOARD);
    XConvertSelection(CLIPBOARD, UTF8_STRING, MINIXCLIPBOARD, g_window);
    
    // Wait for contents in an event loop.
    while (true) {
        XEvent event;
        XNextEvent(&event);
        switch (event.type) {
        case SelectionNotify:
            goto end_wait;
        // often you might handle other events here too
        }
    } end_wait:
    
    // Fetch data and paste.
    char* data;
    Atom type;
    XGetWindowProperty(MINIXCLIPBOARD, &type, &data);
    if (type == UTF8_STRING) // paste
        puts(data);
    else
        fail();
        
    // cleanup
    XFree(data);
    XDeleteProperty(MINIXCLIPBOARD);
}
```

### Copy

To copy from our application, we need to first claim ownership of the selection using `XSetSelectionOwner()`. We also want to double check that we actually got the ownership.

```c
    XSetSelectionOwner(CLIPBOARD, g_window);
    if (g_window != XGetSelectionOwner(CLIPBOARD))
        fail();
```

Now that we have the ownership, we can proceed to become the conceptual "clipboard server". This requires our process to be alive until another client claims ownership, so we'll become a background daemon by forking and killing our parents, so that the shell won't mess with us. 

```c
    if (fork() != 0) // kill parent
        exit(EXIT_SUCCESS);
    chdir("/");
```

Changing working directory to `/` is not very important for us, it just prevents our background process blocking `umount`. 

We are servers now, let's fire up our event loop. Again, we have to handle `SelectionRequest` events by writing the requested data to the property specified in the request using `XChangeProperty()` and respond by sending `XSelectionEvent` back to the requestor window using `XSendEvent()`. We can stop serving once some other X11 client claims clipboard ownership, which we detect by receiving `SelectionClear` event. 

```c
    while (true) { // serve X11 clients requesting clipboard contents.
        XSelectionRequestEvent request;
        XNextEvent(&request);
        
        switch (request.type) {
        case SelectionRequest:
            // Write requested target to property specified 
            XChangeProperty(
                request.requestor, 
                request.property, 
                request.target, // UTF8_STRING assumed
                PropModeReplace, 
                data);
            
            XSelectionEvent reply = {.type = SelectionNotify };
            XSendEvent(request.requestor, &reply);
            break;
            
        case SelectionClear: // somebody else claimed clipboard ownership
            exit(EXIT_SUCCESS);
        }
    }
```

This might already work for some clients. However, we are now just assuming that the requestor always requests UTF8_STRING target. We didn't actually check `request.target` to see if UTF8_STRING is what they are requesting. Furthermore, some clients might ask what targets are we supporting by setting `request.target` to TARGETS. In that case, we must reply with a list of all targets that we support. If we don't, then the requestor won't know that they can request an UTF8_STRING and the copying will fail. Let's modify `case SelectionRequest:` to fix this. 

```c
        case SelectionRequest:
            if (request.target == TARGETS) { // requestor asking supported targets
                Atom targets[] { TARGETS, UTF8_STRING }; // we support these
                XChangeProperty( // store supported targets in requestor's property
                    request.requestor,
                    request.property,
                    request.target, // TARGETS
                    PropModeReplace,
                    targets);
            } else if (request.target == UTF8_STRING && request.property != None)
                // requestor asking for the actual data, store to it's property.
                XChangeProperty(
                    request.requestor, 
                    request.property, 
                    request.target, // UTF8_STRING
                    PropModeReplace, 
                    data);
            
            XSelectionEvent reply = {.type = SelectionNotify };
            XSendEvent(request.requestor, &reply);
            break;
```

Note that we are also checking if `request.property != None` to make sure that there is a property to write the data to. 

Finally, the full pseudocode for copying:

```c
void input_to_clipboard(char data[])
{
    // Claim ownership of CLIPBOARD selection.
    XSetSelectionOwner(CLIPBOARD, g_window);
    if (g_window != XGetSelectionOwner(CLIPBOARD))
        fail();
    
    // Fork to background.
    if (fork() != 0) // kill parent
        exit(EXIT_SUCCESS);
    chdir("/");
    
    // Serve X11 clients requesting clipboard contents.
    while (true) {
        XSelectionRequestEvent request;
        XNextEvent(&request);
        
        switch (request.type) {
        case SelectionRequest:
            if (request.target == TARGETS) { // requestor asking supported targets
                Atom targets[] { TARGETS, UTF8_STRING }; // we support these
                XChangeProperty( // store supported targets in requestor's property
                    request.requestor,
                    request.property,
                    request.target, // TARGETS
                    PropModeReplace,
                    targets);
            } else if (request.target == UTF8_STRING && request.property != None)
                // requestor asking for the actual data, store to it's property.
                XChangeProperty(
                    request.requestor, 
                    request.property, 
                    request.target, // UTF8_STRING
                    PropModeReplace, 
                    data);
            
            XSelectionEvent reply = {.type = SelectionNotify };
            XSendEvent(request.requestor, &reply);
            break;
            
        case SelectionClear: // somebody else claimed clipboard ownership
            exit(EXIT_SUCCESS);
        }
    }
}
```

### Clipboard Manager

Clipboard owners need to make sure that the process is alive as long as they have to serve selection requests. Badly implemented clipboard handling may exit process while being the owner of the clipboard. In such case, the contents of the clipboard will vanish since the clipboard owner also owns the data. We solved this by forking the process in the background, but there is an alternative (and arguably more idiomatic) solution: *clipboard managers*. 

Clipboard managers are dedicated background processes that clipboard owners can transfer ownership and clipboard data to. If the owner wishes to exit application, they can send a `ConvertSelection()` request to CLIPBOARD_MANAGER selection with SAVE_TARGETS target. We are not going to go details on how to do this, because we don't actually recommend doing this, we only mention this for completeness since this method is quite commonly used. 

The reason why we don't recommend clipboard managers is that they have inconsistent implementations and are not guaranteed to exist in all systems. In fact, despite the claims that clipboard managers are ubiquitous, it was not present on the authors system (openSUSE Slowroll with KDE Plasma 6.6) at the time of writing. And indeed, here is a snippet from README of GLFW: 

> Linux and other Unix-like systems running X11 are supported even without a desktop environment or modern extensions, although some features require a clipboard manager or a modern window manager. 

If as ubiquitous library as GLFW is not guaranteed to have a fully working clipboard out of the box due to missing clipboard manager, then we argue that it is not worth bothering with. We used forking to background (like `xclip`), which is more robust and arguably simpler too: if one wishes to exit, then just fork to background and start a small event loop handling selection requests, which is already implemented. 

### Final Notes

Most applications would not directly use the semi-hard-coded atoms like CLIPBOARD and UTF8_STRING. You would usually parameterize these values to a variable instead. 

This program handled copying and pasting completely separately with their own event loops. This makes it very easy to understand how each operation works, but it is likely to be more common for applications to have copying and pasting mixed together in the same event loop. 

We only handled strings. Fully featured clipboard obviously needs to handle other types as well (images, audio, etc.). 

We assumed that data to be transferred (both copy and paste) is relatively small, which is why we read/wrote the data in one step. This is often not the case, notably video files can be quite large and they would be often transferred incrementally. This is done using INCR properties. We'll leave it as an exercise to the reader to figure out the details of this and implementation. 

We used `XNextEvent()` in our event loops, which is blocking. This is probably fine for simple command line application, but often you might want to do work concurrently, especially if transferring large amounts of data incrementally, so a more robust application might prefer to use `XCheckIfEvent()` instead to avoid freezing. 

Readers are encouraged to read the full source code of `minixclip` found in `minixclip.c`, which is only 232 loc. Readers are also encouraged to read the source code of `xclip` (mostly `xclip.c` and parts of `xclib.c`) to see how a fully featured clipboard utility does things right. The relevant files are below 2 kloc total, where a significant portion is just parsing command line arguments, comments, and other boilerplate that can be skipped. 

## Resources

- https://tronche.com/gui/x/xlib/window-information/properties-and-atoms.html
- https://www.chiark.greenend.org.uk/~tthurman/icccm/sec-2.html
- https://handmade.network/forums/articles/t/8544-implementing_copy_paste_in_x11
- https://stackoverflow.com/questions/79838300/how-clipboard-selection-and-atoms-works-in-x11-xlib
- https://www.jwz.org/doc/x-cut-and-paste.html
- https://movq.de/blog/postings/2017-04-02/0/POSTING-en.html
