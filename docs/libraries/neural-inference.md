# Local neural-network inference

LogitOS provides CPU transformer inference through `c/lib/nn` and the `lm` command-line application. A host converter reads GGUF weights and writes the runtime’s `LOGITLM` format. The library describes borrowed model weights, allocates inference state, performs attention and feed-forward computation, and samples tokens. Its memory-sizing interfaces let callers calculate weight and context costs before allocating the inference arena.

## Execution and ownership

The `Makefile` filters `c/lib/nn` out of the kernel source list and compiles it with userland flags. The `lm` application links these objects with mini-libc and the project’s libm objects. Matrix operations execute in the application; kernel services provide file access and memory mappings.

The runtime has three resource lifetimes:

| Resource | Ownership |
|---|---|
| Model blob | The application owns a file mapping or an allocated copy. |
| `struct lm_model` | Borrows weight storage from the blob and owns an array of layer descriptors. |
| `struct lm_state` | Owns an arena containing activations, logits and cached attention keys and values. |

`lm_open()` creates descriptors pointing into the blob. The blob must remain accessible while those descriptors are used. Cleanup in `c/apps/lm/lm.c` releases the state with `lm_state_free()`, releases descriptors with `lm_close()`, and then unmaps or frees the blob.

## From GGUF to LOGITLM

The host converter in `tools/lmshape.c` uses the GGUF reader in `tools/gguf.c` and the runtime’s quantization functions. Its build rule lives in `tests/nn.mk`.

| Converter option | Purpose |
|---|---|
| `--weights` | Read weights and architecture metadata from a GGUF file. |
| `--dtype` | Select f32, q8 or q4 output weights. |
| `--seq` | Set the runtime context capacity. |
| `--out` | Write a model file. |
| `--dry-run` | Print the shape and calculated file size. |
| `--verify` | Open an existing conversion and check its contents. |

`gguf_open()` maps the source file and parses metadata and tensor descriptors. `arch_from_gguf()` reads the Qwen3 dimensions, attention head counts, head width, rotary base and RMS normalization epsilon. It chooses q8 output and a context capacity of 512 when the corresponding options are omitted.

The converter derives flags from the source tensors. An absent output matrix selects tied embeddings; the first layer’s query normalization tensor selects query/key normalization; an embedding stored in a type other than F32 selects storage at the output weight dtype. Qwen3 conversion selects the NeoX rotary coordinate pairing.

Before writing, the converter checks that every destination tensor has a source with matching dimensions and that every source tensor is claimed. GGUF’s first dimension is contiguous, so its matrix dimensions map to the runtime’s column count followed by row count. The writer reads source values row by row and calls the same q8 and q4 quantizers used by the library.

## Model format and loading

`c/lib/nn/model.h` defines a 64-byte `struct lm_header` followed by tensors in a fixed order: token embedding, each layer’s tensors, final normalization gains and, for an untied model, the classifier matrix. Dimensions and flags determine each offset.

| Flag | Meaning |
|---|---|
| `LM_TIED` | The classifier shares the token embedding’s storage. |
| `LM_QKNORM` | Each layer contains query and key normalization gains. |
| `LM_QEMB` | The embedding uses the selected weight dtype. |
| `LM_ROPE_NEOX` | Rotary embeddings pair coordinates across the two halves of a head. |

Normalization gains use f32 storage. The header carries rotary base and RMS epsilon as f32 bit patterns; zero selects `LM_ROPE_BASE_DFL` or `LM_RMS_EPS_DFL`. Keeping these values with the weights preserves the model’s position encoding and normalization arithmetic.

In `c/lib/nn/model.c`, `lm_open()` checks the magic, version, reserved field, architecture constants and header consistency. `lm_expected_size()` validates dimensions and supported flags while checking size arithmetic for overflow. The loader requires the blob length to equal that calculated size before assigning weight pointers.

With `LM_TIED`, the classifier receives a copy of the embedding descriptor, so both refer to the same bytes. `lm_close()` frees the layer descriptor array and clears the model object.

## File mappings and copied weights

`c/apps/lm/lm.c` obtains the model length with `stat()`, opens the file and tries a private mapping with read permission. The libc wrapper in `c/apps/libc/src/mman.c` reaches `SYS_MMAP_FILE`, whose handler is in `c/kernel/mm/mmsys.c`. File faults in `c/kernel/mm/virt/fault.c` obtain pages from the page cache; reclaim can discard cached pages and let subsequent accesses reload them.

Several older comments in `c/apps/lm/lm.c` say libc rejects file mappings and opening a file copies its entire contents into the kernel heap. The current code supports file mappings, and `file_open_vfs()` in `c/kernel/exec/fd/file.c` streams eligible read-only files through offset reads. Some application diagnostics still repeat the older explanation.

The application retains a direct syscall fallback after a failed libc mapping. If mapping fails, or `--read` is supplied, it allocates a buffer for the complete model. On LogitOS, that copy path first tries the whole-file syscall and can fall back to descriptor reads. `--posix-read` selects the descriptor read loop. The returned byte count must match the expected file length.

## Tensors and matrix operations

`struct nn_tensor` in `c/lib/nn/nn.h` holds a dtype, up to four dimensions, payload pointers and an ownership flag. Allocation and wrapping helpers in `c/lib/nn/tensor.c` let callers describe either owned f32/q8 tensors or borrowed storage.

| Dtype | Weight storage |
|---|---|
| `NN_F32` | Contiguous f32 values. |
| `NN_Q8` | Signed bytes with one f32 scale per matrix row. |
| `NN_Q4` | Packed nibbles with a scale and minimum per block. |

For q8, `nn_quantize_q8()` chooses each row’s scale from its largest absolute weight and quantizes to the range −127 through 127. An all-zero row receives a zero scale.

Single-token inference uses matrix-vector products. `nn_matvec_f32()` and `nn_matvec_q8()` in `c/lib/nn/matmul.c` accumulate four lanes with SSE2 operations and reduce them in a fixed order. The q8 kernel widens signed weights to float, multiplies by f32 activations and applies the row scale after reduction. Preserving that reduction order preserves the arithmetic behavior relied on by generation comparisons.

The same file provides `nn_matmul_f32()` for row-major matrix multiplication. Its loop order walks a row of the second input and the output contiguously for each element of the shared dimension.

## Four-bit quantization

The shipped model layout uses `Q4_BLOCK` equal to 64 and `Q4_MODE` equal to `Q4_AFFINE`, defined in `c/lib/nn/quant4.h`. These settings belong to the format version.

For each block, `q4_quantize()` finds the minimum and maximum, derives a scale from their range, and rounds to codes from 0 through 15. Reconstruction is the stored minimum plus the code multiplied by the scale. Constant blocks use a zero scale and retain their value in the minimum.

Blocks stay within matrix rows, with a short block at the end when needed. Low nibbles store the first half of a block’s weights; high nibbles store the second half. This arrangement lets both halves multiply contiguous activation runs.

A tensor contains packed bytes, padding to a multiple of four bytes, the scale array and the minimum array. A complete 64-weight block costs 32 packed bytes plus eight metadata bytes, or five bits per weight. Older comments quote 5.03 bits per weight; five is the complete-block cost implied by the implementation, with short blocks and tensor padding adding overhead.

`q4_matvec()` accumulates products using raw nibble values. `q4_xsum()` computes an activation sum for each block, shared across output rows. The block epilogue combines the raw product sum with the scale and minimum. `q4_mv()` supplies the activation-sum scratch on its stack.

Q4 wrapping places packed bytes in the tensor’s `q` pointer, scales in `scale`, and minima in `data`. Use the q4 sizing helpers for this representation; `q4_mat_bytes()` supplies the size used by the model loader.

The design notes in `c/lib/nn/quant4.h`, dated 2026-08-20, record the accuracy tradeoff behind affine blocks of 64. On two trained 824k-parameter models, the reported held-out loss increases over their own f32 weights were 0.0046 and 0.0163 nats per byte.

## Forward computation and sampling

`lm_forward()` in `c/lib/nn/infer.c` consumes one token at the state’s next position and returns vocabulary logits stored in the state. It checks model identity, token range, position and projection tensor shapes.

Each call performs this sequence:

1. Materialize the token embedding into the residual stream.
2. Apply RMS normalization and project queries, keys and values.
3. Apply any query/key normalization, then rotary position encoding.
4. Store the new keys and values in the layer’s cache.
5. Compute attention scores over positions through the current token, apply softmax and sum cached values.
6. Project the attention output and add it to the residual stream.
7. Apply the second RMS normalization, gate and up projections, SwiGLU, and down projection, then add the result to the residual stream.
8. Apply final normalization and the classifier.

Grouped-query attention maps query heads to KV heads using their head-count ratio. Query width is `n_heads * head_dim`, which can differ from the residual dimension. The query and attention output projections connect those widths.

`c/lib/nn/ops.c` implements normalization, softmax, SwiGLU and rotary operations. RMS normalization accumulates squares in double precision. Softmax subtracts the maximum before exponentiation. For head widths through 256, the forward pass builds a double-precision rotary table once per token and reuses it across layers and heads.

`lm_sample_greedy()` chooses the first maximum. `lm_sample_topp()` applies temperature and nucleus sampling, modifying the logit buffer in place and using random state supplied by the caller.

## KV cache and memory sizing

`lm_state_new()` allocates the inference arena once. The same layout routine counts bytes and assigns pointers, padding sub-buffers to 16-byte boundaries. `lm_state_bytes()` exposes the allocation size, and `lm_state_why()` explains a sizing refusal. Forward calls reuse those buffers.

The active key/value cache consists of two f32 arrays shaped as `[n_layers, seq_len, kv_dim]`, where `kv_dim` is the KV head count multiplied by head width. Before alignment padding, their combined size is:

```c
2 * n_layers * seq_len * kv_dim * sizeof(float)
```

For 28 layers, eight KV heads and a head width of 128:

| Context capacity | K and V storage |
|---|---:|
| 256 tokens | 56 MiB |
| 512 tokens | 112 MiB |
| 2,048 tokens | 448 MiB |
| 32,768 tokens | 7,168 MiB |

`tests/unit/kvbudget_test.c` contains assertions for these values. Attention scores add `n_heads * seq_len` floats, and the arena also holds residuals, projections, feed-forward activations and logits. `lm_state_reset()` starts a new context by setting the next position to zero; attention reaches cache entries through the current position.

`c/lib/nn/kvcache.c` provides another cache representation through `struct lm_kvcache`. Its q8 form stores a scale for each layer, position and KV head, quantizes on write and reconstructs into caller buffers on read.

For file sizing, `lm_expected_size()` accounts for the current model layout. The Qwen3-0.6B shape documented in `c/lib/nn/model.h`, with tied embeddings stored at the selected dtype, has these approximate file sizes:

| Weight dtype | File size |
|---|---:|
| f32 | 2,273.8 MiB |
| q8 | 570.5 MiB |
| q4 | 355.5 MiB |

## Using the lm tool

The `Makefile` packages the application as `/bin/lm`. Its `MODEL_LM` rule selects an optional `model.lm` input from the configured build directory and packages it as `/model.lm`, the application’s default model path.

After installing a model whose tokens represent bytes, run this from a guest shell:

```sh
/bin/lm -p "The kernel" -n 64 --greedy
```

The application echoes the prompt, generates up to 64 further bytes within the context capacity, and prints timing and logit diagnostics.

| Option | Behavior |
|---|---|
| `-m` | Select the model path. |
| `-p` | Feed prompt bytes as token IDs. |
| `-n` | Set the generated-token limit; default 200. |
| `-t` | Set temperature; default 0.8. |
| `-s` | Set the stochastic sampler seed. |
| `--greedy` | Select maximum-logit sampling. |
| `--ids` | Supply up to 512 IDs, separated by commas or spaces. |
| `--print-ids` | Print generated integer IDs. |
| `--dump-logits` | Write raw f32 logits after prompt processing. |
| `--seq` | Lower context capacity before state allocation. |
| `--budget` | Set the application’s memory ceiling in MiB; zero disables it. |
| `--read` | Select copying the complete model. |
| `--posix-read` | Select copying through descriptor reads. |
| `--mm` | Request kernel memory diagnostics at marked stages. |

For Qwen3, `tools/lmtok.py` provides host encoding and decoding. Use `--ids` and `--print-ids` to exchange tokens with the guest application.

Prompt processing calls `lm_forward()` once per token. Generation samples an ID, forwards it, emits it, then samples from the resulting logits. An empty prompt starts generation with token zero. Stochastic generation uses a nucleus threshold of 0.9; an omitted seed comes from `arc4random_buf()`.

In the rich Terminal mode, `lm` emits begin, token and end frames. Its interrupt handler lets the current generation step finish, closes the stream as interrupted and returns status 130.

## Performance and checks

`lm` reports model loading time, total forward steps, first-step latency and the rate of later forward calls. The overall rate includes prompt processing and generation. The later-call rate uses accumulated time inside forward calls after the first call.

Mapped weights bring page faults and storage reads into forward-call timing. The first call touches weight pages; subsequent calls reflect the cache and reclaim conditions of that run. The guest clock used by `lm`, through `c/apps/libc/src/time.c`, advances in 10 ms increments. The application labels shorter runs as too short to time.

The application also scans logit rows for their range and non-finite values. These diagnostics help distinguish arithmetic failures from slow execution.

The historical host benchmark recorded in `c/lib/nn/quant4.h` reports q4 matrix-vector throughput around half the q8 rate across its listed shapes. It demonstrates the cost of unpacking on that host. Guest measurements include the application’s arithmetic, page faults and storage workload.

Nucleus sampling adds work that grows with vocabulary size. `lm_sample_topp()` repeatedly scans the vocabulary to find the next nucleus member, giving a cost of O(V × K) for vocabulary size V and nucleus size K.

`tests/nn.mk` defines these checks:

| Target | Coverage |
|---|---|
| `test-nn` | Tensor kernels, numerical references and negative controls. |
| `test-lm-format` | Model layout, borrowed pointers and loader refusals. |
| `test-lm-infer` | Forward results against a double-precision reference, state geometry and rotary controls. |
| `test-lm-units` | Shape calculations, q4 operations and cache checks. |
| `test-gguf` | Conversion, verification, source reconstruction and matrix orientation. |
| `test-qwen-logits` | Converted-model logits against an external reference workflow. |
| `test-lm-os` | Host and guest generation from the same application source and model. |

The fragment includes capability guards and missing-input skips for the targets that use them.

## Limitations

- GGUF conversion accepts the `qwen3` architecture, versions 2 and 3, and F32, F16 and Q8_0 source tensors. Q8_0 requires a contiguous dimension divisible by 32. Query and value head widths must match. See `tools/gguf.c` and `tools/lmshape.c`.
- The forward pass processes one token per call on the CPU. The application stops generation at the requested count or context capacity and has no end-token check. See `c/lib/nn/infer.c` and `c/apps/lm/lm.c`.
- The active inference state uses an f32 KV cache. The q8 cache in `c/lib/nn/kvcache.c` has its own interface and is not connected to `lm_forward()`.
- The current `lm` build links libc’s default 24 MiB heap reservation and commit ceiling, with smaller reservations possible after fallback. A 56 MiB inference state exceeds that heap even with mapped weights. Changing `--budget` leaves the allocator’s limits in force. See `Makefile` and `c/apps/libc/src/malloc.c`.
- The default 256 MiB application budget counts state bytes plus copied model bytes. Mapped weights contribute zero to that calculation, which excludes resident weight pages, descriptor allocations and other process or kernel costs. `--mm` requests kernel observations. See `c/apps/lm/lm.c`.
- The q4 weight branch of `lm_budget_compute()` uses symmetric block storage and omits the file header, undercounting current affine q4 files. Use `lm_expected_size()` for file bytes and `lm_state_bytes()` for the active arena. See `c/lib/nn/kvcache.c`.
- `q4_mv()` allows at most 1,024 activation-sum blocks, covering input widths through 65,536 at the shipped block size. See `c/lib/nn/quant4.c`.

## Source map

| File | What it contains |
|---|---|
| `c/lib/nn/nn.h` | Tensor descriptors and arithmetic interfaces. |
| `c/lib/nn/tensor.c` | Tensor allocation, wrapping and release. |
| `c/lib/nn/matmul.c` | SSE2 matrix-vector kernels, f32 matrix multiplication and q8 conversion. |
| `c/lib/nn/ops.c` | Normalization, softmax, SwiGLU and rotary operations. |
| `c/lib/nn/quant4.h` | Q4 constants, interfaces and historical design measurements. |
| `c/lib/nn/quant4.c` | Packing, quantization, sizing and matrix-vector execution. |
| `c/lib/nn/model.h` | LOGITLM header, flags, payload order and model descriptors. |
| `c/lib/nn/model.c` | File sizing, validation, weight views and embedding lookup. |
| `c/lib/nn/infer.h` | Inference state and execution interfaces. |
| `c/lib/nn/infer.c` | Arena layout, forward pass, active KV cache and sampling. |
| `c/lib/nn/kvcache.h` | Cache and memory-budget interfaces. |
| `c/lib/nn/kvcache.c` | F32/q8 cache storage and budget calculations. |
| `c/apps/lm/lm.c` | CLI, loading, generation, streaming and diagnostics. |
| `tools/gguf.c` | GGUF parsing and source-weight reconstruction. |
| `tools/lmshape.c` | Conversion, tensor mapping, sizing and verification. |
| `tools/lmtok.py` | Host tokenizer encoding and decoding. |
| `c/apps/libc/src/mman.c` | Application memory-mapping wrappers. |
| `c/apps/libc/src/malloc.c` | Heap reservation and allocation limits. |
| `c/apps/libc/src/time.c` | Clock implementation used by application timing. |
| `c/kernel/exec/fd/file.c` | Descriptor opening and offset reads. |
| `c/kernel/mm/mmsys.c` | File-mapping syscall handling. |
| `c/kernel/mm/virt/fault.c` | File faults that map cached pages. |
| `c/kernel/mm/reclaim/reclaim.c` | Reclamation of cached file pages. |
| `Makefile` | Userland compilation, linking and disk packaging. |
| `tests/nn.mk` | Host tool rules, inference checks and controls. |
| `tests/unit/kvbudget_test.c` | Cache-size assertions and cache comparisons. |

## See also

- [Libraries](overview.md)
- [The C library](../userland/libc.md)
- [Memory model](../architecture/memory-model.md)
- [Page cache, reclaim and swap](../kernel/page-cache-and-reclaim.md)
- [Testing](../architecture/testing.md)