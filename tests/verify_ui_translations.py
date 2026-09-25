"""Check English source text and complete Chinese translations without Qt."""

import json
import re
import xml.etree.ElementTree as ET
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HAN = re.compile(r"[\u3400-\u9fff]")
TOKENS = re.compile(r'//[^\n]*|/\*.*?\*/|R"\((.*?)\)"|"(?:\\.|[^"\\])*"', re.S)
TRANSLATE = re.compile(
    r'QCoreApplication::translate\(\s*"([^"]+)"\s*,\s*'
    r'((?:"(?:\\.|[^"\\])*"\s*)+)\)'
)
QT_TR = re.compile(r'(?<![\w:])tr\(\s*((?:"(?:\\.|[^"\\])*"\s*)+)\)')
QOBJECT_CLASS = re.compile(r'class\s+([A-Za-z_]\w*)\b[^\{]*\{\s*Q_OBJECT\b', re.S)
NAMESPACE = re.compile(r'namespace\s+([A-Za-z_]\w*)\s*\{')
PLACEHOLDERS = re.compile(r"%(?:L?[1-9][0-9]*|n)")


def main():
    errors = []
    catalog = ET.parse(ROOT / "src/ui/translations/cohavora_zh_CN.ts")
    if catalog.getroot().get("sourcelanguage") != "en_US":
        errors.append("Catalog source language must be en_US")
    entries = {}
    for context in catalog.findall("context"):
        for message in context.findall("message"):
            source = message.findtext("source", "")
            translation = message.find("translation")
            translated = message.findtext("translation", "")
            key = (context.findtext("name"), source)
            if key in entries:
                errors.append(f"Duplicate translation key: {key}")
            entries[key] = translated
            if HAN.search(source):
                errors.append(f"Chinese source text: {source}")
            if translation is None or not translated or translation.get("type"):
                errors.append(f"Incomplete Chinese translation: {source}")
            if sorted(PLACEHOLDERS.findall(source)) != sorted(PLACEHOLDERS.findall(translated)):
                errors.append(f"Translation placeholders differ: {source}")

    references = set()
    qt_contexts = {}
    for header in (ROOT / "src").rglob("*.h"):
        if "tdesktop" in header.parts:
            continue
        code = header.read_text(encoding="utf-8")
        class_match = QOBJECT_CLASS.search(code)
        if not class_match:
            continue
        namespace_match = NAMESPACE.search(code, 0, class_match.start())
        context = class_match[1]
        if namespace_match:
            context = f"{namespace_match[1]}::{context}"
        qt_contexts[header.with_suffix("")] = context

    for path in (ROOT / "src").rglob("*"):
        if path.suffix not in (".cpp", ".h") or "tdesktop" in path.parts:
            continue
        code = path.read_text(encoding="utf-8")
        # Skip comments, but check literals in both UI and native diagnostics
        # because the native Room forwards its diagnostics to the UI console.
        for token in TOKENS.finditer(code):
            if token[0].startswith("/"):
                continue
            if token[1] is not None:
                text = token[1]
            else:
                # Native code also contains C++ escapes such as \0 and \xNN.
                # Decode universal character escapes without interpreting
                # UTF-8 text through a different character encoding.
                text = re.sub(r'\\(?:u([0-9a-fA-F]{4})|U([0-9a-fA-F]{8}))',
                              lambda m: chr(int(m[1] or m[2], 16)), token[0])
            if HAN.search(text):
                line = code.count("\n", 0, token.start()) + 1
                errors.append(f"Chinese runtime literal: {path.relative_to(ROOT)}:{line}")
        for match in TRANSLATE.finditer(code):
            source = "".join(json.loads(s) for s in re.findall(r'"(?:\\.|[^"\\])*"', match[2]))
            key = (match[1], source)
            references.add(key)
            if key not in entries:
                errors.append(f"Missing Chinese catalog entry: {key}")
        tr_matches = list(QT_TR.finditer(code))
        if tr_matches:
            context = qt_contexts.get(path.with_suffix(""))
            if not context:
                errors.append(f"Unable to infer Qt tr context: {path.relative_to(ROOT)}")
                continue
            for match in tr_matches:
                source = "".join(json.loads(s) for s in re.findall(
                    r'"(?:\\.|[^"\\])*"', match[1]))
                key = (context, source)
                references.add(key)
                if key not in entries:
                    errors.append(f"Missing Chinese catalog entry: {key}")

    for key in entries.keys() - references:
        errors.append(f"Obsolete catalog entry: {key}")
    if errors:
        raise SystemExit("\n".join(errors))
    print(f"PASS: {len(entries)} English translation keys, complete Chinese translations, "
          "matching placeholders, and no Chinese runtime literals in application sources.")


if __name__ == "__main__":
    main()
