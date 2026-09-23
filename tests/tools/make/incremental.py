#!/usr/bin/env python3
"""Check make's incremental decisions using a small, genuinely compiled sample.

Compiler-produced .d files are the consumer oracle. We change no source mtimes:
make -W models edits, and all objects, depfiles and three ELF scheduling sentinels
live in a temporary BUILD. The sentinels and -o-frozen libraries test the link
graph only; this does not claim that the three applications link or boot.
"""
import argparse
from dataclasses import dataclass
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[3]
BROWSER = 'c/apps/gui/browser/'
AS_RUNTIME = 'c/apps/as/runtime/'


@dataclass(frozen=True)
class Sample:
    source: str
    obj: str


def native_samples():
    manifest = (ROOT / AS_RUNTIME / 'sources.def').read_text()
    units = re.findall(r'^AT_RUNTIME_SOURCE\(([a-z][a-z0-9_]*)\)$',
                       manifest, re.MULTILINE)
    if not units or len(units) != len(set(units)):
        raise AssertionError('Aether native source manifest is empty or repeats a unit')
    return tuple(Sample(AS_RUNTIME + unit + '.c',
                        'aether-toolchain/obj/' + unit + '.o') for unit in units)


NATIVE_SAMPLES = native_samples()
SAMPLES = (
    Sample(BROWSER + 'html/tokenizer.c', 'browserobj/' + BROWSER + 'html/tokenizer.o'),
    Sample(BROWSER + 'layout/block.c', 'browserobj/' + BROWSER + 'layout/block.o'),
    Sample(BROWSER + 'html/tree.c', 'browserobj/' + BROWSER + 'html/tree.o'),
    Sample(BROWSER + 'js/style/cssom.c', 'jsobj/' + BROWSER + 'js/style/cssom.o'),
    Sample('c/apps/as/common/version.c', 'asobj/c/apps/as/common/version.o'),
    Sample('c/apps/libc/src/string.c', 'asobj/c/apps/libc/src/string.o'),
    Sample('c/lib/gfx/adapters/openlogit_display.c', 'apps/gfx_adapters/openlogit_display.o'),
    Sample('c/lib/gfx/geometry/gfx_math.c', 'apps/gfx_geometry/gfx_math.o'),
    Sample('c/lib/video/mpeg4_hdr.c', 'vidobj/c/lib/video/mpeg4_hdr.o'),
    Sample('c/lib/video/h264.c', 'vidobj/c/lib/video/h264.o'),
    Sample('c/lib/audio/opus.c', 'audobj/c/lib/audio/opus.o'),
    Sample('c/lib/audio/aac.c', 'audobj/c/lib/audio/aac.o'),
    Sample('c/lib/nn/kvcache.c', 'lmnnobj/c/lib/nn/kvcache.o'),
    Sample('c/lib/nn/infer.c', 'lmnnobj/c/lib/nn/infer.o'),
    Sample('c/apps/lm/lm.c', 'lmobj/c/apps/lm/lm.o'),
    Sample('c/apps/gui/studio/core/document.c', 'apps/gui/studio/core/document.o'),
    Sample('c/apps/gui/studio/core/source_snapshot.c', 'apps/gui/studio/core/source_snapshot.o'),
    *(Sample(f'c/apps/gui/{name}/{name}.c', f'apps/{name}.o')
      for name in ('studio', 'preview', 'terminal')),
    *NATIVE_SAMPLES,
)


def joined(text):
    # Rules and shell commands both use continuations; treating physical lines
    # as rules silently drops all but the first link prerequisite.
    return re.sub(r'\\\r?\n[ \t]*', ' ', text)


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def commands(text):
    # Nested GNU Make writes make[1]: `target' is up to date. The diagnostic's
    # asymmetric quotes are not shell syntax; tokenize only output commands.
    return [shlex.split(line) for line in joined(text).splitlines()
            if re.search(r'(?:^|\s)-o\s', line)]


def compiles(text):
    return {words[words.index('-o') + 1] for words in commands(text)
            if '-c' in words and '-o' in words}


def links(text):
    return {words[words.index('-o') + 1] for words in commands(text)
            if '-o' in words and '-c' not in words and '-nostdlib' in words}


class Probe:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.build = self.directory / 'build'
        self.objects = [str(self.build / sample.obj) for sample in SAMPLES]
        self.native_objects = {str(self.build / sample.obj) for sample in NATIVE_SAMPLES}
        self.deps = {}

    def make(self, goals, *, changed=(), frozen=(), overlay=None, actual=False,
             database=False):
        command = ['make', '--no-print-directory', '-f', 'Makefile']
        if overlay:
            command += ['-f', str(overlay)]
        if not actual:
            command.append('-n')
        if database:
            command.append('-p')
        command += [f'BUILD={self.build}']
        for path in sorted(frozen):
            command += ['-o', path]
        for path in changed:
            # -W of a missing generated object can interact with implicit-rule
            # search differently. Every edited input in this gate really exists.
            require((ROOT / path).exists(), f'-W input is absent: {path}')
            command += ['-W', path]
        command += list(goals)
        env = {key: value for key, value in os.environ.items()
               if key not in ('MAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES')}
        result = subprocess.run(command, cwd=ROOT, env=env, text=True,
                                capture_output=True, timeout=120)
        require(result.returncode == 0, 'make failed:\n' + result.stderr[-5000:])
        return result.stdout

    def compile_samples(self):
        output = self.make(self.objects, actual=True)
        require(compiles(output) == set(self.objects),
                'fresh sample BUILD did not compile exactly the requested objects')
        for sample, obj in zip(SAMPLES, self.objects):
            path = Path(obj)
            require(path.read_bytes().startswith(b'\x7fELF'), f'not a real ELF object: {obj}')
            depfile = path.with_suffix('.d')
            require(depfile.is_file(), f'compiler did not produce {depfile}')
            first = joined(depfile.read_text()).splitlines()[0]
            target, prerequisites = first.split(':', 1)
            require(target == obj, f'depfile targets a different object: {target}')
            self.deps[obj] = set(shlex.split(prerequisites))
            require(sample.source in self.deps[obj], f'depfile lost {sample.source}')
        require(not compiles(self.make(self.objects, actual=True)),
                'unchanged real make recompiled a sample object')
        self.expect('unchanged sample', self.objects, set())
        print(f'build-incremental: compiled {len(self.objects)} real objects and depfiles', flush=True)

    def expect(self, label, goals, expected, *, changed=(), frozen=(), overlay=None,
               expected_links=()):
        output = self.make(goals, changed=changed, frozen=frozen, overlay=overlay)
        actual = compiles(output)

        def relative(paths):
            return sorted(os.path.relpath(path, self.build) for path in paths)

        require(actual == set(expected),
                f'{label}: compile set differs; extra={relative(actual - set(expected))}; '
                f'missing={relative(set(expected) - actual)}')
        require(links(output) == set(expected_links), f'{label}: link set differs')
        return output

    def consumers(self, header):
        return {obj for obj, deps in self.deps.items() if header in deps}

    def check_header(self, header, overlay=None):
        expected = self.consumers(header)
        require(expected and len(expected) < len(self.objects),
                f'header probe lacks consumers or nonconsumers: {header}')
        self.expect(header, self.objects, expected, changed=[header], overlay=overlay)
        return expected

    def link_inputs(self):
        goals = [str(self.build / f'{name}.elf') for name in ('studio', 'preview', 'terminal')]
        output = self.make(goals, database=True)
        rules = {}
        members = set()
        model_main = str(self.build / 'lmobj/c/apps/lm/lm.o')
        model_link = str(self.build / 'lm.elf')
        model_deps = None
        for line in joined(output).splitlines():
            inventory = re.match(
                r'^(?:BROWSER_OBJ|BROWSER_JS_OBJ|AS_OBJ|AS_NATIVE_OBJS|GFX_OBJ|STUDIO_OBJ|VID_OBJ|AUD_OBJ|LM_NN_OBJ)\s*:=\s*(.*)',
                line,
            )
            if inventory:
                members.update(shlex.split(inventory.group(1)))
            if line.startswith(tuple(goal + ':' for goal in goals)):
                target, prerequisites = line.split(':', 1)
                rules[target] = set(shlex.split(prerequisites)) - {'|'}
            if line.startswith(model_link + ':'):
                model_deps = set(shlex.split(line.split(':', 1)[1])) - {'|'}
        require(set(rules) == set(goals), 'make database omitted an application link rule')
        require(model_deps is not None and model_main in model_deps,
                'make database omitted the shipped model application main object')
        members.add(model_main)
        shared = str(self.build / 'apps/gfx_geometry/gfx_math.o')
        main = {str(self.build / f'apps/{name}.o') for name in ('studio', 'preview', 'terminal')}
        # A pattern rule can compile a path the product never links. In
        # particular browser/net/cache.c is textually included, not a standalone
        # BROWSER_OBJ; such a probe would measure an invented translation unit.
        require(set(self.objects) - main <= members,
                'sample object is absent from the product source inventories')
        frozen = set()
        for goal, deps in rules.items():
            require(shared in deps, f'{goal} no longer links the chosen GFX sample')
            require(str(self.build / f'apps/{Path(goal).stem}.o') in deps,
                    f'{goal} does not depend on its separately compiled main object')
            frozen.update(deps - main - {shared})
            # These are deliberately non-ELF timestamp sentinels, never presented
            # as linked products and never consumed by an executing link recipe.
            Path(goal).write_text('incremental graph sentinel, not a linked application\n')
        self.expect('unchanged application graph', goals, set(), frozen=frozen)
        return goals, shared, frozen

    def check_links(self, inputs, overlay=None):
        goals, shared, frozen = inputs
        self.expect('library-only update', goals, set(), changed=[shared],
                    frozen=frozen, overlay=overlay, expected_links=goals)
        if overlay:
            return
        headers = {'studio': 'c/apps/gui/studio/core/studio_render.inc',
                   'preview': 'c/apps/gui/preview/view.inc',
                   'terminal': 'c/apps/coreutils/logit_cells.h'}
        for name in ('studio', 'preview', 'terminal'):
            source = f'c/apps/gui/{name}/{name}.c'
            goal = str(self.build / f'{name}.elf')
            obj = str(self.build / f'apps/{name}.o')
            self.expect(f'{name} source update', [goal], {obj}, changed=[source],
                        frozen=frozen, expected_links=[goal])
            header = headers[name]
            require(header in self.deps[obj], f'{name} main depfile omitted {header}')
            self.expect(f'{name} included header update', [goal], {obj}, changed=[header],
                        frozen=frozen, expected_links=[goal])

    def check_missing_depfile(self):
        # An old .o with no .d must not become permanently current once the
        # coarse prerequisite lists disappear. Recover one object per family,
        # including codec and model objects whose broad header edges are gone.
        for source in (BROWSER + 'html/tokenizer.c', 'c/lib/video/mpeg4_hdr.c',
                       'c/lib/audio/opus.c', 'c/lib/nn/kvcache.c', 'c/apps/lm/lm.c',
                       AS_RUNTIME + 'allocation.c'):
            obj = str(self.build / next(sample.obj for sample in SAMPLES
                                        if sample.source == source))
            depfile = Path(obj).with_suffix('.d')
            before = depfile.read_bytes()
            depfile.unlink()
            self.expect(f'missing depfile for {source}', self.objects, {obj})
            output = self.make([obj], actual=True)
            require(compiles(output) == {obj}, f'missing .d did not rebuild {source}')
            require(depfile.read_bytes() == before,
                    f'recovery did not restore the real depfile for {source}')
            self.expect(f'recovered depfile for {source}', self.objects, set())

    def check_diagnostic_fallback(self):
        # Two video diagnostic objects are outside product.mk's missing-.d
        # inventory. Keep their broad header fallback until that changes, so a
        # surviving old object cannot ignore header edits after losing its .d.
        for name in ('vidbench', 'vidcheck265'):
            obj = str(self.build / f'vidobj/tests/guest/video/{name}.o')
            output = self.make([obj], actual=True)
            require(compiles(output) == {obj}, f'{name} did not compile a real object')
            require(Path(obj).read_bytes().startswith(b'\x7fELF'), f'{name} is not ELF')
            depfile = Path(obj).with_suffix('.d')
            require(depfile.is_file(), f'{name} compiler omitted its depfile')
            depfile.unlink()
            self.expect(f'{name} missing-depfile header fallback', [obj], {obj},
                        changed=['c/lib/video/mpeg4.h'])

    def overlay(self, name, text):
        path = self.directory / (name + '.mk')
        path.write_text(text)
        return path

    def reject(self, name, operation):
        try:
            operation()
        except AssertionError as error:
            # Only the same semantic oracle rejecting excess compilation counts.
            # A make syntax failure or missing tool is a broken control, not PASS.
            require('compile set differs; extra=[' in str(error)
                    and 'extra=[]' not in str(error), f'{name}: wrong failure: {error}')
            print(f'build-incremental negative control: {name}: {error}', flush=True)
            return
        raise AssertionError(f'{name}: legacy dependency unexpectedly passed')

    def controls(self, inputs):
        mutations = (
            ('browser-wide headers', '$(BROWSER_OBJ) $(BROWSER_JS_OBJ): $(BROWSER_HEADERS) $(BROWSER_INCLUDES)\n',
             BROWSER + 'html/entities.inc'),
            ('AS headers on libc', f'{self.build}/asobj/c/apps/libc/src/string.o: $(AS_HDRS)\n',
             'c/apps/as/common/numeric.h'),
            ('AS native-wide headers', '$(AS_NATIVE_OBJS): $(AS_NATIVE_HEADERS)\n',
             AS_RUNTIME + 'allocation.h'),
            ('GFX-wide headers', '$(GFX_OBJ): $(GFX_HEADERS)\n',
             'c/lib/gfx/adapters/display_pixels.h'),
            ('Studio-wide headers', '$(STUDIO_OBJ): $(wildcard c/apps/gui/studio/core/*.h)\n',
             'c/apps/gui/studio/core/runner.h'),
            ('video-wide headers', '$(VID_OBJ): $(wildcard c/lib/video/*.h)\n',
             'c/lib/video/mpeg4.h'),
            ('audio-wide headers', '$(AUD_OBJ): $(wildcard c/lib/audio/*.h)\n',
             'c/lib/audio/opus.h'),
            ('model-wide headers',
             '$(LM_NN_OBJ) $(BUILD)/lmobj/c/apps/lm/lm.o: $(wildcard c/lib/nn/*.h)\n',
             'c/lib/nn/kvcache.h'),
        )
        for name, text, header in mutations:
            overlay = self.overlay(name.replace(' ', '-'), text)
            if name == 'AS native-wide headers':
                # This control restores the actual old rule over every runtime
                # object, not just the one header consumer sampled by the
                # positive oracle. If this count changes, inspect the manifest.
                scheduled = compiles(self.make(self.objects, changed=[header], overlay=overlay))
                require(scheduled & self.native_objects == self.native_objects,
                        'old Aether header edge did not schedule every native object')
                print(f'build-incremental negative control: old Aether edge schedules '
                      f'{len(self.native_objects)} native compiles', flush=True)
            self.reject(name, lambda: self.check_header(header, overlay))

        # Preserve the real link recipes and put the old inline compilation
        # back before them. Thus the negative control differs in exactly the
        # behavior the link-only oracle forbids, not in a toy link graph.
        lines = joined((ROOT / 'Makefile').read_text()).splitlines()
        parts = []
        for name in ('studio', 'preview', 'terminal'):
            prefix = f'$(BUILD)/{name}.elf:'
            recipe = []
            for index, line in enumerate(lines):
                if line.startswith(prefix):
                    candidate = []
                    for following in lines[index + 1:]:
                        if not following.startswith('\t'):
                            break
                        candidate.append(following)
                    if candidate:
                        recipe = candidate
            require(recipe, f'cannot locate the real {name} link recipe')
            flags = '$(UCFLAGS) $(PREVIEW_CF)' if name == 'preview' else '$(UCFLAGS)'
            include = ' -Ic/apps/as' if name == 'studio' else ''
            parts += [prefix,
                      f'\t$(CC) {flags} -c c/apps/gui/{name}/{name}.c -o $(BUILD)/apps/{name}.o{include}',
                      *recipe]
        overlay = self.overlay('inline-main-compile', '\n'.join(parts) + '\n')
        self.reject('inline application compile', lambda: self.check_links(inputs, overlay))


def check(control):
    with tempfile.TemporaryDirectory(prefix='logitos-build-incremental-') as directory:
        probe = Probe(directory)
        probe.compile_samples()
        inputs = probe.link_inputs()
        if control:
            probe.controls(inputs)
            return
        for header in (BROWSER + 'html/entities.inc', BROWSER + 'layout/block.h',
                       'c/apps/as/common/numeric.h', AS_RUNTIME + 'allocation.h',
                       AS_RUNTIME + 'command.h', 'c/lib/gfx/adapters/display_pixels.h',
                       'c/apps/gui/studio/core/runner.h', 'c/lib/video/mpeg4.h',
                       'c/lib/audio/opus.h', 'c/lib/nn/kvcache.h',
                       'c/lib/nn/infer.h'):
            consumers = probe.check_header(header)
            print(f'build-incremental: {header}: {len(consumers)} sampled consumers', flush=True)
        probe.check_links(inputs)
        probe.check_missing_depfile()
        probe.check_diagnostic_fallback()
        print('build-incremental: PASS: selective headers; three library-only links; '
              'source/header edits compile and link; missing depfiles recovered')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--control', action='store_true')
    args = parser.parse_args()
    try:
        check(args.control)
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        raise SystemExit(f'build-incremental: FAIL: {error}')
