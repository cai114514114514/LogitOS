/* Host-test shim for linking the Rust staticlib (rust/) into a host binary:
 * the host's prebuilt libcore references the unwind personality lang-item even
 * though our crate is panic=abort and the parsers never panic -- so this stub
 * exists only to satisfy the linker and can never fire. */
void rust_eh_personality(void) {}
