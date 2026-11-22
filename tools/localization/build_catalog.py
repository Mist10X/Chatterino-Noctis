"""Build the reviewable Russian UI catalog; verify formatting and content boundaries."""
import collections
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).parent

def cpp_hex(text):
    # C++ UTF-8 byte escapes occur in a few existing labels (arrows, middots).
    return re.sub(r'(?:\\x[0-9a-fA-F]{2})+', lambda m: bytes.fromhex(m[0].replace('\\x', '')).decode('utf-8'), text)

def decode_target(text):
    # Prefix labels can have meaningful trailing space; encode it explicitly
    # in review TSVs rather than losing it to editors or whitespace cleanup.
    return text.replace('\\n', '\n').replace('\\t', '\t').replace('\\u0020', ' ')

def build():
    source = list(json.loads((HERE / 'inventory.json').read_text('utf-8')))
    translations = {}
    approved_unchanged = []
    for path in sorted(HERE.glob('ru-*.tsv')):
        for line in path.read_text('utf-8').splitlines():
            if not line or line.startswith('#'):continue
            raw, value = line.split('\t', 1)
            index = int(raw)
            assert index not in translations, (path.name, index, 'duplicate')
            if value == '=':
                value = source[index];approved_unchanged.append(index)
            translations[index] = decode_target(value)
    assert set(translations) == set(range(len(source))), ('Missing ids', set(range(len(source))) - translations.keys())
    result = {cpp_hex(s): translations[i] for i, s in enumerate(source)}
    def supplement(source_file, translations_file):
        keys = json.loads((HERE / source_file).read_text('utf-8'))
        entries = {}
        for line in (HERE / translations_file).read_text('utf-8').splitlines():
            if not line or line.startswith('#'):continue
            number, target = line.split('\t', 1)
            key = keys[int(number)]
            assert key not in entries, ('Duplicate supplemental key', key)
            entries[key] = key if target == '=' else decode_target(target)
        assert set(entries) == set(keys), ('Missing supplemental translations', set(keys)-entries.keys())
        return entries
    result.update(supplement('extra-en-sources.json', 'extra-ru.tsv'))
    result.update(supplement('final-en-sources.json', 'final-ru.tsv'))
    english = supplement('existing-ru-sources.json', 'existing-en.tsv')
    english.update(json.loads((HERE / 'extra-en.json').read_text('utf-8')))
    branding_path = HERE / 'branding-overrides.json'
    branding = json.loads(branding_path.read_text('utf-8')) if branding_path.exists() else {}
    # Numbered inventories use the original source text. Resolve those keys
    # once while building the catalogs so widgets use the current English
    # spelling even before a translator is installed.
    source_renames = branding.get('en', {})
    english = {key: source_renames.get(value, value)
               for key, value in english.items()}
    result.update({key:key for key in english})
    review_sources = list(json.loads((HERE / 'review-needed.json').read_text('utf-8')))
    for path in sorted(HERE.glob('review-*.tsv')):
        for line in path.read_text('utf-8').splitlines():
            if not line:continue
            number, target = line.split('\t', 1)
            result[cpp_hex(review_sources[int(number)])] = decode_target(target)
    extra_path = HERE / 'extra-ru.json'
    if extra_path.exists():result.update(json.loads(extra_path.read_text('utf-8')))
    result.update(branding.get('ru', {}))
    for old, new in source_renames.items():
        if old != new and old in result:
            translated = result.pop(old)
            result.setdefault(new, translated)
    for key, value in [*result.items(), *english.items()]:
        assert value, ('Empty translation', key)
        assert collections.Counter(re.findall(r'%L?\d+|%n', key)) == collections.Counter(re.findall(r'%L?\d+|%n', value)), ('Placeholder mismatch', key, value)
        assert re.findall(r'(?:href|src)=[\'\"](.*?)[\'\"]', key) == re.findall(r'(?:href|src)=[\'\"](.*?)[\'\"]', value), ('Link mismatch', key)
        assert collections.Counter(re.findall(r'\{[A-Za-z][\w.]*\}', key)) == collections.Counter(re.findall(r'\{[A-Za-z][\w.]*\}', value)), ('Command variable mismatch', key)
    for language, entries in [('en', english), ('ru', result)]:
        output = ROOT / 'resources/i18n' / (language + '.json')
        serialized = json.dumps(entries, ensure_ascii=False, indent=2) + '\n'
        if not output.exists() or output.read_text('utf-8') != serialized:
            output.write_text(serialized, 'utf-8')
    summary = dict(entries=len(result), source_literals=len(source), unchanged_technical_or_brand_ids=approved_unchanged,
                   changed=sum(key != value for key, value in result.items()))
    (HERE / 'coverage.json').write_text(json.dumps(summary, indent=2), 'utf-8')
    print(json.dumps(summary))
    return result

if __name__ == '__main__':build()
