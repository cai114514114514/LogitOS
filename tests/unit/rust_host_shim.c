/* Host-test shim for linking the Rust staticlib (rust/) into a host binary:
 * the host's prebuilt libcore references the unwind personality lang-item even
 * though our crate is panic=abort and the parsers never panic -- so this stub
 * exists only to satisfy the linker and can never fire. */
void rust_eh_personality(void) {}

/* The image-decoder registration callbacks. The Rust staticlib is one codegen
 * unit, so a host test that wants only the inflater still drags png_register --
 * and therefore whatever png_register calls -- into its link.
 *
 * That is why this list is an ABI for every host link domain and not just the
 * image one, and it is how it went wrong: 4952ceb (APNG) changed png_register
 * from img_register to img_register_anim and updated three of the host tests
 * that link this staticlib. http1_test.c was missed and turned `make
 * test-browser` red at a link step nobody had touched; this shim was missed too
 * and took test-stream, test-stream-control and test-cookie-cors with it. Two
 * separate lines each found one half and each reported it as "not mine".
 *
 * Only img_register_anim is here. img_register is NOT: the tests that link this
 * shim define it themselves, and adding it produced `multiple definition of
 * img_register` -- the mirror-image failure of the one being fixed. The shim
 * fills what the staticlib needs and nobody supplies, not everything it needs.
 *
 * Never called: registration only records a decoder, and no host test that
 * links this shim decodes an image. */
void img_register_anim(void *d, void *f, void *a) { (void)d; (void)f; (void)a; }
