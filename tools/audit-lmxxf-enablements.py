"""Collect upstream integration evidence and require a commit-bound human/agent review.

This is a conservative text inventory, not a C++/PowerShell interpreter. Equal-looking
assignments do not prove a feature is wired; names containing TEST do not prove irrelevance.
Every upstream changed path is also listed, including files outside our vendor closure.
Exit codes: 0 = valid review (or explicit --report-only), 3 = review needed, 1 = audit error.
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CONFIG = Path('tools/lmxxf-sync')
VENDOR = Path('third_party/lmxxf')
RUNTIME = Path('OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr')
OPTIONS = RUNTIME / 'backend/lmxxf_runtime/LmxxfProductionOptions.h'
PROFILES = ('scripts/hip-game-flags.txt', 'scripts/hip-re9-flags.txt')


def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=True,
                                     separators=(',', ':')).encode()).hexdigest()


def file_hash(path):
    # All review inputs are text; checkout CRLF/BOM differences must not invalidate evidence.
    return hashlib.sha256(path.read_text(encoding='utf-8-sig').encode('utf-8')).hexdigest()


def read_json(path):
    # Reject duplicate keys instead of accepting a last-one-wins review/configuration.
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f'Duplicate JSON key: {key}')
            result[key] = value
        return result
    return json.loads(path.read_text(encoding='utf-8-sig'), object_pairs_hook=unique)


class Git:
    def __init__(self, directory):
        self.directory = Path(directory).resolve()
        self.prefix = ['git', '-c', 'safe.directory=' + self.directory.as_posix(),
                       '-C', str(self.directory)]
        self.cache = {}

    def run(self, *args, allowed=(0,)):
        result = subprocess.run(self.prefix + list(args), stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        if result.returncode not in allowed:
            raise RuntimeError('git ' + ' '.join(args) + ': ' +
                               result.stderr.decode('utf-8', 'replace').strip())
        return result.stdout.decode('utf-8', 'strict')

    def resolve(self, ref):
        value = self.run('rev-parse', '--verify', '--end-of-options', ref + '^{commit}').strip()
        if not re.fullmatch('[0-9a-f]{40}', value):
            raise ValueError(f'Invalid commit: {value}')
        return value

    def tree(self, commit):
        result = {}
        for entry in self.run('ls-tree', '-r', '-z', commit).split('\0'):
            if entry:
                info, path = entry.split('\t', 1)
                mode, kind, blob = info.split()
                result[path] = {'mode': mode, 'kind': kind, 'blob': blob}
        return result

    def read(self, commit, path):
        key = (commit, path)
        if key not in self.cache:
            self.cache[key] = self.run('show', commit + ':' + path)
        return self.cache[key]

    def grep(self, commit, *paths):
        # Exit 1 means no matches; errors (missing refs, permissions, etc.) remain fatal.
        body = self.run('grep', '-n', '-I', '-E', '(DLSS5_|HIP_)[A-Z0-9_]+', commit,
                        '--', *paths, allowed=(0, 1))
        rows = []
        for line in body.splitlines():
            match = re.match(r'^[0-9a-f]{40}:(.*?):([0-9]+):(.*)$', line)
            if not match:
                raise ValueError(f'Unrecognized git grep row: {line[:160]}')
            rows.append((match[1], int(match[2]), match[3]))
        return rows


def assignments(text):
    """Inventory literal, chained, false and non-boolean assignments; retain expressions."""
    code = re.sub(r'/\*.*?\*/|//[^\n]*', ' ', text, flags=re.S)
    result = {}
    for match in re.finditer(r'((?:\bo\.[a-zA-Z0-9_]+\s*=(?!=)\s*)+)([^;]+);', code):
        value = re.sub(r'\s+', ' ', match[2]).strip()
        for name in re.findall(r'o\.([a-zA-Z0-9_]+)', match[1]):
            result.setdefault(name, set()).add(value)
    return {name: sorted(values) for name, values in sorted(result.items())}


def parse_recipe(text):
    rows = {}
    pattern = r"@\{\s*name\s*=\s*'([^']+)'\s*;\s*defines\s*=\s*@\(([^)]*)\)\s*;\s*sources\s*=\s*@\(([^)]*)\)\s*\}"
    for match in re.finditer(pattern, text):
        name = match[1]
        if name in rows:
            raise ValueError(f'Duplicate recipe module: {name}')
        sources = re.findall(r"'([^']+)'", match[3])
        if not sources or any(not re.fullmatch(r'[A-Za-z0-9_.-]+\.hip', s) for s in sources):
            raise ValueError(f'Unrecognized sources in module {name}; update audit parser')
        rows[name] = {'sources': sources, 'defines': re.findall(r"'([^']+)'", match[2])}
    # Do not silently drop a row when upstream changes its recipe syntax.
    if not rows or len(rows) != len(re.findall(r'@\{\s*name\s*=', text)):
        raise ValueError('Unrecognized build recipe rows; update the audit parser before reviewing')
    return rows


def local_inputs(root, manifest):
    files = {CONFIG / 'manifest.json', CONFIG / 'module-defines.json',
             Path('tools/sync-lmxxf-upstream.ps1'), Path('tools/audit-lmxxf-enablements.py'),
             Path('tools/lmxxf-module-package.ps1')}
    files.update(CONFIG / 'patches' / spec['patch'] for spec in manifest['pinned'])
    files.add(CONFIG / 'patches/reference-network.patch')
    files.update(path.relative_to(root) for path in (root / CONFIG).glob('*.ps1'))
    files.update(VENDOR / path for path in manifest['headers'])
    files.update(path.relative_to(root) for path in (root / VENDOR / 'hip').glob('*.hip'))
    files.update(VENDOR / 'hip' / name for name in ('build-modules.ps1', 'rtc_compile.cpp'))
    files.update(path.relative_to(root) for path in (root / VENDOR / 'shaders').glob('*.hlsl'))
    files.update(path.relative_to(root) for path in (root / RUNTIME).rglob('*')
                 if path.is_file() and path.suffix in ('.h', '.hpp', '.cpp', '.inl'))
    files.add(OPTIONS)
    return {path.as_posix(): file_hash(root / path) for path in sorted(files)}


def collect(root, git, base, commit, skipped, supplied_modules=None):
    manifest = read_json(root / CONFIG / 'manifest.json')
    overrides = read_json(root / CONFIG / 'module-defines.json')
    trees = {ref: git.tree(ref) for ref in (base, commit)}
    tree = trees[commit]
    net = git.read(commit, 'src/native_hip_network.h')
    production = git.read(commit, 'src/LmxxfProductionOptions.h')
    recipe = parse_recipe(git.read(commit, 'hip/build-modules.ps1'))
    for name, defines in overrides.items():
        if name not in recipe:
            raise ValueError(f'Local module override has no upstream recipe row: {name}')
        for define in defines:
            if not re.fullmatch(r'HIP_[A-Z0-9_]+ [0-9]+', define):
                raise ValueError(f'Invalid local module define: {define}')
    ours = assignments((root / OPTIONS).read_text(encoding='utf-8-sig'))
    upstream_options = assignments(production)
    conditional_options = assignments(net)
    inputs = local_inputs(root, manifest)
    items = []

    def item(key, kind, evidence):
        items.append({'id': key, 'kind': kind, 'evidence': evidence,
                      'fingerprint': digest(evidence)})

    item('integration', 'integration', {
        'from_commit': base, 'to_commit': commit, 'local_inputs': digest(inputs),
        'required': 'Read upstream.diff and source consumers; trace Options -> runtime/ABI -> module selection -> recipe -> kernel. Record staged integration and actual validation.'})
    # Include every changed upstream path. New code/configuration outside the copy list must
    # not disappear merely because the switch scanner does not yet understand its spelling.
    changed = git.run('diff', '--name-status', '--no-renames', '-z', base, commit, '--').split('\0')
    for offset in range(0, len(changed) - 1, 2):
        status, path = changed[offset:offset + 2]
        item('file:' + path, 'upstream-change', {'path': path, 'status': status,
             'before': trees[base].get(path), 'after': tree.get(path)})

    for name in sorted(set(upstream_options) | set(conditional_options) | set(ours)):
        direct = upstream_options.get(name, [])
        conditional = conditional_options.get(name, [])
        local = ours.get(name, [])
        if direct != local or any(value not in local for value in conditional):
            item('option:' + name, 'runtime-option', {'field': name, 'upstream_product': direct,
                'upstream_host_assignments': conditional, 'local': local,
                'source': ['src/LmxxfProductionOptions.h', 'src/native_hip_network.h', OPTIONS.as_posix()]})

    flags = {}
    for profile in PROFILES:
        for number, line in enumerate(git.read(commit, profile).splitlines(), 1):
            match = re.fullmatch(r'\s*(DLSS5_[A-Z0-9_]+)\s*=\s*(.*?)\s*', line)
            if match:
                flags.setdefault(match[1], []).append({'path': profile, 'line': number, 'value': match[2]})
            elif line.strip().startswith('DLSS5_'):
                raise ValueError(f'Unrecognized profile entry: {profile}:{number}')
    for name in re.findall(r'\bDLSS5_[A-Z0-9_]+', net):
        flags.setdefault(name, [])
    consumers = {}
    for path, number, line in git.grep(commit, 'src', 'Development/HIP'):
        # Lab experiments are evidence, not a production consumer.
        if '/experiments/' in path or path.endswith(('.md', '.patch', '.json')):
            continue
        for name in set(re.findall(r'\bDLSS5_[A-Z0-9_]+', line)):
            if name in flags:
                consumers.setdefault(name, {})[path] = tree[path]['blob']
    # This is only a navigation aid. It never declares a flag integrated or safe to enable.
    mappings = {}
    segments = re.split(r'(?=(?:std::)?getenv\(")', net)
    for segment in segments:
        match = re.match(r'(?:std::)?getenv\("(DLSS5_[A-Z0-9_]+)"\)', segment)
        if match:
            mappings.setdefault(match[1], set()).update(re.findall(r'\bo\.([a-zA-Z0-9_]+)\s*=', segment))
    for name, values in sorted(flags.items()):
        fields = sorted(mappings.get(name, []))
        item('flag:' + name, 'runtime-flag', {'flag': name, 'profiles': values,
            'candidate_fields': fields, 'local_assignments': {field: ours.get(field, []) for field in fields},
            'consumer_blobs': consumers.get(name, {}),
            'note': 'Classify by actual consumer and deployment evidence, including disabled/numeric/test flags; never by prefix alone.'})

    deployment = {}
    for path, number, line in git.grep(commit, 'Development/deployments', 'Development/HIP/experiments'):
        for name in set(re.findall(r'\bHIP_[A-Z0-9_]+', line)):
            deployment.setdefault(name, []).append({'path': path, 'line': number, 'text': line.strip(),
                                                   'blob': tree[path]['blob']})
    gates = {}
    for module, row in sorted(recipe.items()):
        defs = row['defines'] + overrides.get(module, [])
        for source in row['sources']:
            path = 'hip/' + source
            body = git.read(commit, path)  # Missing recipe input is an audit failure, not noise.
            names = set()
            for directive in re.findall(r'^\s*#\s*(?:if|ifdef|ifndef|elif|define)\b[^\n]*', body, re.M):
                names.update(re.findall(r'\bHIP_[A-Z0-9_]+', directive))
            for name in sorted(names):
                defaults = re.findall(r'^\s*#\s*define\s+' + re.escape(name) + r'\s+([^\n]+)', body, re.M)
                gates.setdefault(name, []).append({'module': module, 'source': path, 'blob': tree[path]['blob'],
                    'recipe_and_local_defines': [d for d in defs if d.split()[0] == name],
                    'source_definitions': defaults,
                    'implicit_recipe_candidate': name == 'HIP_ISA_HALF' or (name == 'HIP_PREPACKED_WEIGHTS' and module.endswith('-packed'))})
    for name, uses in sorted(gates.items()):
        item('kernel:' + name, 'kernel-gate', {'macro': name, 'per_module': uses,
            'deployment_evidence': [row for row in deployment.get(name, []) if row['path'].startswith('Development/deployments/')],
            'experiment_evidence': [row for row in deployment.get(name, []) if '/experiments/' in row['path']]})

    pinned_diff = []
    for spec in manifest['pinned']:
        path = spec['path']
        local = (root / VENDOR / path).read_text(encoding='utf-8-sig')
        raw = git.read(commit, path) if path in tree else ''
        pinned_diff.extend(difflib.unified_diff(raw.splitlines(True), local.splitlines(True),
                           fromfile='upstream/' + path, tofile='local/' + path))
        item('pinned:' + path, 'pinned-header', {'path': path, 'upstream': tree.get(path),
             'local_sha256': file_hash(root / VENDOR / path), 'update_switch': spec['switch'],
             'note': 'Inspect pinned-headers.diff; retaining a header can miss new API/features even when its local markers pass.'})
    if supplied_modules is not None:
        bundle = Path(supplied_modules)
        binaries = {}
        for path in sorted(bundle.rglob('*.hsaco')):
            if path.is_file():
                rel = path.relative_to(bundle).as_posix()
                if rel.startswith('/') or '..' in rel:
                    raise ValueError(f'Unsafe module path: {rel}')
                binaries[rel] = hashlib.sha256(path.read_bytes()).hexdigest()
        if not binaries:
            raise ValueError(f'No external modules to review: {bundle}')
        meta_names = {'SHA256SUMS', 'modules.json', 'runtime-manifest.json', 'README.md'}
        metadata = {}
        for path in sorted(bundle.rglob('*')):
            if path.is_file() and path.name in meta_names:
                rel = path.relative_to(bundle).as_posix()
                if rel.startswith('/') or '..' in rel:
                    raise ValueError(f'Unsafe metadata path: {rel}')
                metadata[rel] = file_hash(path)
        item('modules:external', 'external-modules', {'binaries': binaries, 'metadata': metadata,
             'required': 'Record build source/defines/toolchain provenance and validation; hashes alone do not prove source equivalence.'})
    for check in sorted(set(skipped)):
        item('validation:' + check, 'validation-exception', {'check': check,
             'required': 'Explain the exception and record the remaining build/module validation as a next step.'})
    report = {'schema': 1, 'from_commit': base, 'to_commit': commit,
              'local_inputs': inputs, 'items': items}
    report['snapshot'] = digest(report)
    return report, ''.join(pinned_diff)


def template(report, previous=None):
    previous = previous or {}
    carry = {}
    if previous.get('to_commit') in (report['from_commit'], report['to_commit']):
        carry = {entry['id']: entry for entry in previous.get('decisions', []) if isinstance(entry, dict) and 'id' in entry}
    decisions = []
    for item in report['items']:
        old = carry.get(item['id'], {})
        entry = {'id': item['id'], 'fingerprint': item['fingerprint'], 'classification': 'pending',
                 'decision': 'pending', 'reason': '', 'evidence': '', 'validation': '', 'next_step': ''}
        if old.get('fingerprint') == item['fingerprint']:
            for key in ('classification', 'decision', 'reason', 'evidence', 'validation', 'next_step'):
                entry[key] = old.get(key, entry[key])
        decisions.append(entry)
    return {'schema': 1, 'from_commit': report['from_commit'], 'to_commit': report['to_commit'],
            'snapshot': report['snapshot'], 'reviewer': '', 'summary': '', 'validation': '', 'decisions': decisions}


def nonempty(value):
    return isinstance(value, str) and bool(value.strip()) and value.strip().lower() not in ('todo', 'tbd', 'pending')


def validate_review(report, review):
    problems = []
    for key in ('schema', 'from_commit', 'to_commit', 'snapshot'):
        if review.get(key) != report[key]:
            problems.append(f'Review {key} is stale or missing; use the new template and recheck changed evidence')
    for key in ('reviewer', 'summary', 'validation'):
        if not nonempty(review.get(key)):
            problems.append(f'Review needs {key}')
    entries = review.get('decisions', [])
    if not isinstance(entries, list) or any(not isinstance(e, dict) for e in entries):
        return problems + ['Review decisions must be an array of objects']
    by_id = {entry.get('id'): entry for entry in entries}
    if len(by_id) != len(entries):
        problems.append('Duplicate review decision IDs')
    expected = {item['id'] for item in report['items']}
    if set(by_id) - expected:
        problems.append('Review contains obsolete/unknown decision IDs')
    for item in report['items']:
        key = item['id']
        entry = by_id.get(key, {})
        if entry.get('fingerprint') != item['fingerprint']:
            problems.append(f'{key}: evidence changed or decision missing')
        if entry.get('classification') not in ('production', 'experiment', 'out-of-scope'):
            problems.append(f'{key}: classify production / experiment / out-of-scope')
        decision = entry.get('decision')
        if decision not in ('integrated', 'deferred', 'excluded'):
            problems.append(f'{key}: choose integrated / deferred / excluded')
        for field in ('reason', 'evidence'):
            if not nonempty(entry.get(field)):
                problems.append(f'{key}: needs {field}')
        if decision == 'integrated' and not nonempty(entry.get('validation')):
            problems.append(f'{key}: integrated requires actual validation evidence')
        if (decision == 'deferred' or item['kind'] == 'validation-exception') and not nonempty(entry.get('next_step')):
            problems.append(f'{key}: needs an actionable next_step')
    return problems


def write_reports(directory, report, review_template, diff, pinned_diff):
    directory.mkdir(parents=True, exist_ok=True)
    for name, value in (('report.json', report), ('review.template.json', review_template)):
        (directory / name).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    (directory / 'upstream.diff').write_text(diff, encoding='utf-8')
    (directory / 'pinned-headers.diff').write_text(pinned_diff, encoding='utf-8')
    rows = ['# Upstream integration review', '',
            f"Range: `{report['from_commit']}` -> `{report['to_commit']}`", '',
            'Read `upstream.diff`, `pinned-headers.diff` and the evidence in `report.json`.',
            'This is a text inventory, not proof that a branch executes or an optimization is enabled.',
            'Classify test/production paths from consumers, then trace runtime -> module -> kernel.',
            'Do not bulk-approve or enable test flags. Record staged work and actual validation.', '',
            'Copy `review.template.json` to the review path, fill each decision, and rerun sync.',
            'Unchanged item decisions can be carried into the template; the overall validation must be refreshed.', '',
            '| Item | Kind |', '|---|---|']
    rows.extend(f"| `{item['id']}` | {item['kind']} |" for item in report['items'])
    (directory / 'report.md').write_text('\n'.join(rows) + '\n', encoding='utf-8')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('upstream', nargs='?', help='Author clone; default is beside the primary checkout')
    parser.add_argument('ref', nargs='?', default='origin/main')
    parser.add_argument('--base', help='Previously completed upstream commit; defaults to UPSTREAM.md')
    parser.add_argument('--output-dir', type=Path, default=ROOT / 'exports/lmxxf-upstream')
    parser.add_argument('--review', type=Path, default=ROOT / VENDOR / 'upstream-review.json')
    parser.add_argument('--skipped-check', action='append', default=[])
    parser.add_argument('--modules-path', type=Path, help='Bind review to an externally supplied module bundle')
    parser.add_argument('--report-only', action='store_true', help='Collect evidence only; does not certify integration')
    args = parser.parse_args(argv)
    if not args.upstream:
        common = Path(Git(ROOT).run('rev-parse', '--path-format=absolute', '--git-common-dir').strip())
        args.upstream = str(common.parent.parent / 'dlss5-on-amd-9070xt-porting')
    git = Git(args.upstream)
    commit = git.resolve(args.ref)
    base = args.base
    if not base:
        pin = re.findall(r'^- Commit: `([0-9a-f]{40})`', (ROOT / VENDOR / 'UPSTREAM.md').read_text(encoding='utf-8'), re.M)
        if len(pin) != 1:
            raise ValueError('Cannot read prior commit from UPSTREAM.md; supply --base')
        base = pin[0]
    base = git.resolve(base)
    report, pinned_diff = collect(ROOT, git, base, commit, args.skipped_check, args.modules_path)
    previous = read_json(args.review) if args.review.is_file() else None
    review_template = template(report, previous)
    diff = git.run('diff', '--no-ext-diff', '--no-textconv', '--no-renames', base, commit, '--')
    write_reports(args.output_dir, report, review_template, diff, pinned_diff)
    print(f"Audit collected {len(report['items'])} review items for {commit}.")
    print(f'Report: {args.output_dir / "report.md"}')
    print(f'Template: {args.output_dir / "review.template.json"}')
    if args.report_only:
        print('REPORT ONLY: integration has not been approved.')
        return 0
    problems = validate_review(report, previous or {})
    if problems:
        print(f'REVIEW REQUIRED: {len(problems)} unresolved checks. Sources are not certified integrated.')
        for problem in problems[:12]:
            print('  ' + problem)
        print(f'Complete {args.review}; see the template for all decisions.')
        return 3
    deferred = sum(entry['decision'] == 'deferred' for entry in previous['decisions'])
    print(f'Integration review verified; {deferred} explicit deferrals remain in the plan.')
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError, RuntimeError, KeyError, TypeError) as error:
        print(f'AUDIT ERROR: {error}', file=sys.stderr)
        sys.exit(1)
