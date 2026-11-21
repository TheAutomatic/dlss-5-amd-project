#!/usr/bin/env python3
"""Static contract check: every FindName/SetLabel name used by the launcher C# exists in MainWindow.xaml
with a compatible element type, and every x:Name of the baseline XAML is still present."""
import os, re, sys, subprocess, xml.etree.ElementTree as ET, pathlib

# Usage: check-launcher-xaml.py [BASE_REF]   (default: origin/v2.0, override with LAUNCHER_XAML_BASE)
src = pathlib.Path(__file__).resolve().parent.parent / 'install' / 'launcher'
base_ref = sys.argv[1] if len(sys.argv) > 1 else os.environ.get('LAUNCHER_XAML_BASE', 'origin/v2.0')
xaml = src / 'MainWindow.xaml'
X = '{http://schemas.microsoft.com/winfx/2006/xaml}Name'

def names_of(text):
    root = ET.fromstring(text)
    out = {}
    for el in root.iter():
        n = el.get(X) or el.get('Name')
        if n and not el.tag.endswith('}Setter'):
            tag = el.tag.split('}')[-1]
            out.setdefault(n, []).append(tag)
    return out

cur = names_of(xaml.read_text(encoding='utf-8-sig'))
base_text = subprocess.run(['git', '-C', str(src), 'show', base_ref + ':tools/install/launcher/MainWindow.xaml'],
                           capture_output=True, text=True, encoding='utf-8', check=True).stdout
base = names_of(base_text.lstrip('\ufeff'))

# Template-internal names (inside ControlTemplate) are not reachable by Window.FindName; track separately.
def template_names(text):
    root = ET.fromstring(text)
    inner = set()
    for t in root.iter():
        if t.tag.split('}')[-1] in ('ControlTemplate', 'DataTemplate'):
            for el in t.iter():
                n = el.get(X) or el.get('Name')
                if n: inner.add(n)
    return inner

cur_tpl = template_names(xaml.read_text(encoding='utf-8-sig'))
base_tpl = template_names(base_text.lstrip('\ufeff'))

errors = []
for n in ('ProfilesExpandButton', 'ProfilesSummary', 'ProfilesChevron'):
    if n in cur:
        errors.append('OBSOLETE OptiScaler collapse control: %s' % n)
baseline_renames = {'OpenRrAuthorButton': 'OpenRrAsiButton'}
for n, tags in base.items():
    if n in base_tpl:
        continue
    current_name = baseline_renames.get(n, n)
    if current_name not in cur or current_name in cur_tpl:
        errors.append('MISSING baseline x:Name %s (%s)' % (n, tags[0]))
    elif cur[current_name][0] != tags[0]:
        errors.append('TYPE CHANGED %s: %s -> %s' % (n, tags[0], cur[current_name][0]))

for n, tags in cur.items():
    if len(tags) > 1 and n not in cur_tpl:
        errors.append('DUPLICATE x:Name %s' % n)

# Type compatibility of C# casts
derived = {
    'FrameworkElement': None,  # anything
    'UIElement': None,
    'Control': {'Button', 'CheckBox', 'RadioButton', 'ComboBox', 'TextBox', 'ListBox', 'ScrollViewer', 'Expander', 'ProgressBar', 'ToggleButton'},
    'ContentControl': {'Button', 'CheckBox', 'RadioButton', 'Expander', 'ScrollViewer', 'ToggleButton'},
    'ButtonBase': {'Button', 'CheckBox', 'RadioButton', 'ToggleButton'},
    'ToggleButton': {'CheckBox', 'RadioButton', 'ToggleButton'},
    'Panel': {'Grid', 'StackPanel', 'WrapPanel', 'DockPanel', 'UniformGrid', 'Canvas'},
    'Decorator': {'Border'},
    'Shape': {'Ellipse', 'Rectangle', 'Path'},
    'Selector': {'ComboBox', 'ListBox'},
    'ItemsControl': {'ComboBox', 'ListBox'},
    'TextElement': {'Run', 'Hyperlink'},
    'Inline': {'Run', 'Hyperlink'},
}
cast_re = re.compile(r'\(\s*([A-Za-z_.]+)\s*\)\s*(?:_?window|_window|window|dialog)\.FindName\(\s*"([^"]+)"\s*\)')
as_re = re.compile(r'(?:_?window|_window|window)\.FindName\(\s*"([^"]+)"\s*\)\s+as\s+([A-Za-z_.]+)')
plain_re = re.compile(r'(?:_window|window)\.FindName\(\s*"([^"]+)"\s*\)')
label_re = re.compile(r'SetLabel\(\s*"([^"]+)"')
used = {}
for f in sorted(src.glob('*.cs')):
    t = f.read_text(encoding='utf-8-sig')
    for m in cast_re.finditer(t):
        used.setdefault(m.group(2), set()).add((m.group(1).split('.')[-1], f.name))
    for m in as_re.finditer(t):
        used.setdefault(m.group(1), set()).add((m.group(2).split('.')[-1], f.name))
    for m in plain_re.finditer(t):
        used.setdefault(m.group(1), set()).add(('object', f.name))
    for m in label_re.finditer(t):
        used.setdefault(m.group(1), set()).add(('label', f.name))

# names created dynamically in dialogs are not part of MainWindow
dialog_only = set()
for n, uses in sorted(used.items()):
    if n not in cur or n in cur_tpl:
        # SetLabel silently tolerates missing names, but flag it anyway
        errors.append('FindName/SetLabel target not in MainWindow.xaml: %s used by %s' % (n, sorted(uses)))
        continue
    tag = cur[n][0]
    for typ, fn in uses:
        if typ in ('object', 'label', tag):
            continue
        ok = derived.get(typ, set())
        if ok is None or tag in ok:
            continue
        errors.append('TYPE MISMATCH %s: XAML %s, C# casts to %s (%s)' % (n, tag, typ, fn))

print('baseline names: %d, current names: %d, C# referenced names: %d' % (len(base), len(cur), len(used)))
if errors:
    print('\n'.join(errors))
    sys.exit(1)
print('CONTRACT OK')
