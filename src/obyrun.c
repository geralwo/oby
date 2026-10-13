/* obyrun — run a bundled bytecode image.
 *
 * This is the binary `oby --bundle` copies and appends an image to: the VM, the
 * native library and the image loader, with no compiler, no prelude sources and
 * no CLI flags. Since a build of it carries nothing to bundle, it refuses
 * source archives — those are the toolchain's dev path. */
#include <stdio.h>
#include <stdlib.h>
#include "core/bundle.h"
#include "core/os.h"
#include "vm/image.h"
#include "vm/vm.h"

int main(int argc, char *argv[]) {
    (void)argc;

    char path[4096];
    if (!os_get_executable_path(argv[0], path, sizeof path)) {
        fprintf(stderr, "error: cannot find the runner's own path\n");
        return 1;
    }

    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return 1;
    }

    BundleFooter footer;
    if (!bundle_read_footer(file, &footer)) {
        fprintf(stderr, "error: '%s' carries no bundle; it runs bundled images only\n",
                path);
        fclose(file);
        return 1;
    }
    if (footer.payload_type != OBY_BUNDLE_TYPE_IMAGE) {
        fprintf(stderr, "error: '%s' does not hold a bytecode image (payload type %u)\n",
                path, footer.payload_type);
        fclose(file);
        return 1;
    }

    unsigned char *payload = bundle_read_payload(file, &footer);
    fclose(file);
    if (!payload) {
        fprintf(stderr, "error: cannot read the bundle payload\n");
        return 1;
    }

    Image image;
    if (!image_load(payload, (size_t)footer.payload_len, &image)) {
        fprintf(stderr,
                "error: cannot load the bundle image; was it built by a different runner?\n");
        free(payload);
        return 1;
    }
    free(payload);

    VM vm;
    vm_init(&vm, image.global_size);
    int status = vm_run(&vm, image.entry);
    vm_free(&vm);
    image_free(&image);
    return status;
}
