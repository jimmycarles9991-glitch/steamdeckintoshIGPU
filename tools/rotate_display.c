// Request a display transform through the active IOFramebuffer.
// Build on macOS with:
//   clang -Wall -Wextra -framework IOKit -framework ApplicationServices \
//     tools/rotate_display.c -o /tmp/rotate-display
// Usage: /tmp/rotate-display 270   (or 0 to restore normal orientation)

#include <ApplicationServices/ApplicationServices.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

enum { kIOFBSetTransform = 0x00000400 };

static io_service_t builtinDisplayFramebuffer(CGDirectDisplayID *selectedDisplay) {
    CGDisplayCount count = 0;
    if (CGGetOnlineDisplayList(0, NULL, &count) != kCGErrorSuccess || count == 0) return IO_OBJECT_NULL;

    CGDirectDisplayID *displays = calloc(count, sizeof(*displays));
    if (displays == NULL) return IO_OBJECT_NULL;
    if (CGGetOnlineDisplayList(count, displays, &count) != kCGErrorSuccess) {
        free(displays);
        return IO_OBJECT_NULL;
    }

    io_service_t framebuffer = IO_OBJECT_NULL;
    const CGDirectDisplayID mainDisplay = CGMainDisplayID();
    for (CGDisplayCount i = 0; i < count; ++i) {
        const CGDirectDisplayID display = displays[i];
        if (!CGDisplayIsBuiltin(display)) continue;
        if (display != mainDisplay && framebuffer != IO_OBJECT_NULL) continue;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        framebuffer = CGDisplayIOServicePort(display);
#pragma clang diagnostic pop
        if (framebuffer != IO_OBJECT_NULL && selectedDisplay != NULL) *selectedDisplay = display;
        if (framebuffer != IO_OBJECT_NULL && display == mainDisplay) break;
    }
    free(displays);
    return framebuffer;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <0|90|180|270>\n", argv[0]);
        return 2;
    }

    char *end = NULL;
    errno = 0;
    const long degrees = strtol(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0' ||
        (degrees != 0 && degrees != 90 && degrees != 180 && degrees != 270)) {
        fprintf(stderr, "rotation must be 0, 90, 180, or 270 degrees\n");
        return 2;
    }

    IOOptionBits transform = kIOScaleRotate0;
    if (degrees == 90) transform = kIOScaleRotate90;
    else if (degrees == 180) transform = kIOScaleRotate180;
    else if (degrees == 270) transform = kIOScaleRotate270;

    CGDirectDisplayID selectedDisplay = 0;
    const io_service_t framebuffer = builtinDisplayFramebuffer(&selectedDisplay);
    if (framebuffer == IO_OBJECT_NULL) {
        fprintf(stderr, "could not find the built-in display's IOFramebuffer service\n");
        return 1;
    }

    const double before = CGDisplayRotation(selectedDisplay);
    const IOOptionBits options = kIOFBSetTransform | (transform << 16);
    const kern_return_t result = IOServiceRequestProbe(framebuffer, options);
    if (result != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceRequestProbe failed: 0x%x\n", result);
        return 1;
    }
    double after = CGDisplayRotation(selectedDisplay);
    for (unsigned int attempt = 0; attempt < 40 && after != (double)degrees; ++attempt) {
        usleep(50000);
        after = CGDisplayRotation(selectedDisplay);
    }
    printf("display 0x%x: rotation %.0f -> %.0f degrees (requested %ld)\n",
           selectedDisplay, before, after, degrees);
    if (after != (double)degrees) {
        fprintf(stderr, "framebuffer accepted the request but did not report the requested rotation\n");
        return 1;
    }
    return 0;
}
