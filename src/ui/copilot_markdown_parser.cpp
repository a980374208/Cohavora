#include "src/ui/copilot_markdown_parser.h"

#include <QtCore/QSet>
#include <QtCore/QUrl>

#include <algorithm>

namespace OpenMeeting {
namespace {

bool isSafeExternalOrInternalUrl(const QString &raw) {
	const QUrl url(raw.trimmed(), QUrl::StrictMode);
	if (!url.isValid() || url.isRelative()) {
		return false;
	}
	const QString scheme = url.scheme().toLower();
	if (scheme == QStringLiteral("https") || scheme == QStringLiteral("http")) {
		return true;
	}
	if (scheme == QStringLiteral("cohavora")) {
		const QString host = url.host().toLower();
		const QString path = url.path().toLower();
		if (host == QStringLiteral("retry")) {
			return (path.isEmpty() || path == QStringLiteral("/")) &&
				!url.hasQuery() && !url.hasFragment();
		}
		if (host == QStringLiteral("settings")) {
			static const QSet<QString> kAllowedPaths = {
				QString(),
				QStringLiteral("/"),
				QStringLiteral("/general"),
				QStringLiteral("/video"),
				QStringLiteral("/audio"),
				QStringLiteral("/security"),
				QStringLiteral("/copilot"),
				QStringLiteral("/copilot/asr"),
				QStringLiteral("/copilot/translation"),
				QStringLiteral("/about"),
			};
			return kAllowedPaths.contains(path) && !url.hasQuery() && !url.hasFragment();
		}
	}
	return false;
}

bool isIdentifierChar(QChar ch) {
	return ch.isLetterOrNumber() || ch == QLatin1Char('_');
}

void appendParagraphSeparator(QString &out, bool doubleGap = false) {
	if (out.isEmpty()) {
		return;
	}
	if (!out.endsWith(QLatin1Char('\n'))) {
		out.append(QLatin1Char('\n'));
	}
	if (doubleGap && !out.endsWith(QStringLiteral("\n\n"))) {
		out.append(QLatin1Char('\n'));
	}
}

void appendWithShift(TextWithEntities &dst, const TextWithEntities &src) {
	if (src.text.isEmpty() && src.entities.isEmpty()) {
		return;
	}
	const int base = dst.text.size();
	dst.text.append(src.text);
	for (EntityInText entity : src.entities) {
		entity.shiftRight(base);
		dst.entities.push_back(std::move(entity));
	}
}

void finalizeRichText(TextWithEntities &rich) {
	while (rich.text.endsWith(QLatin1Char('\n'))) {
		rich.text.chop(1);
	}
	for (auto &entity : rich.entities) {
		entity.updateTextEnd(rich.text.size());
	}
	rich.entities.erase(
		std::remove_if(rich.entities.begin(), rich.entities.end(), [](const EntityInText &e) {
			return e.length() <= 0;
		}),
		rich.entities.end());

	std::stable_sort(rich.entities.begin(), rich.entities.end(), [](const EntityInText &a, const EntityInText &b) {
		if (a.offset() != b.offset()) {
			return a.offset() < b.offset();
		}
		return a.length() > b.length();
	});
}

bool matchFenceOpen(const QString &line, QChar &outChar, int &outCount, QString &outLang) {
	int i = 0;
	while (i < line.size() && i < 3 && line.at(i) == QLatin1Char(' ')) {
		++i;
	}
	if (i >= line.size()) {
		return false;
	}
	const QChar marker = line.at(i);
	if (marker != QLatin1Char('`') && marker != QLatin1Char('~')) {
		return false;
	}
	int count = 0;
	while (i + count < line.size() && line.at(i + count) == marker) {
		++count;
	}
	if (count < 3) {
		return false;
	}
	const QString rest = line.mid(i + count).trimmed();
	if (marker == QLatin1Char('`') && rest.contains(QLatin1Char('`'))) {
		return false;
	}
	outChar = marker;
	outCount = count;
	outLang = rest;
	return true;
}

bool matchFenceClose(const QString &line, QChar expectedChar, int minCount) {
	const QString trimmed = line.trimmed();
	if (trimmed.size() < minCount) {
		return false;
	}
	for (const QChar ch : trimmed) {
		if (ch != expectedChar) {
			return false;
		}
	}
	return true;
}

bool matchHeading(const QString &line, QString &outText) {
	int level = 0;
	while (level < line.size() && level < 6 && line.at(level) == QLatin1Char('#')) {
		++level;
	}
	if (level == 0 || level >= line.size() || !line.at(level).isSpace()) {
		return false;
	}
	outText = line.mid(level + 1).trimmed();
	return !outText.isEmpty();
}

bool matchHorizontalRule(const QString &line) {
	const QString trimmed = line.trimmed();
	if (trimmed.size() < 3) {
		return false;
	}
	const QChar marker = trimmed.at(0);
	if (marker != QLatin1Char('-') && marker != QLatin1Char('*') && marker != QLatin1Char('_')) {
		return false;
	}
	int count = 0;
	for (const QChar ch : trimmed) {
		if (ch == marker) {
			++count;
		} else if (!ch.isSpace()) {
			return false;
		}
	}
	return count >= 3;
}

bool matchUnorderedList(const QString &line, int &outIndent, QString &outBody) {
	int i = 0;
	while (i < line.size() && line.at(i) == QLatin1Char(' ')) {
		++i;
	}
	if (i + 1 >= line.size()) {
		return false;
	}
	const QChar ch = line.at(i);
	if ((ch != QLatin1Char('-') && ch != QLatin1Char('*') && ch != QLatin1Char('+')) ||
		line.at(i + 1) != QLatin1Char(' ')) {
		return false;
	}
	outIndent = i;
	outBody = line.mid(i + 2);
	return true;
}

bool matchOrderedList(const QString &line, int &outIndent, QString &outPrefix, QString &outBody) {
	int i = 0;
	while (i < line.size() && line.at(i) == QLatin1Char(' ')) {
		++i;
	}
	const int numStart = i;
	while (i < line.size() && (i - numStart) < 4 && line.at(i).isDigit()) {
		++i;
	}
	if (i == numStart || i + 1 >= line.size()) {
		return false;
	}
	const QChar sep = line.at(i);
	if ((sep != QLatin1Char('.') && sep != QLatin1Char(')')) ||
		line.at(i + 1) != QLatin1Char(' ')) {
		return false;
	}
	outIndent = numStart;
	outPrefix = line.mid(numStart, i - numStart + 1) + QLatin1Char(' ');
	outBody = line.mid(i + 2);
	return true;
}

bool lineHasUnescapedPipe(const QString &line) {
	for (int i = 0; i < line.size(); ++i) {
		if (line.at(i) == QLatin1Char('\\') && i + 1 < line.size()) {
			++i;
			continue;
		}
		if (line.at(i) == QLatin1Char('|')) {
			return true;
		}
	}
	return false;
}

QStringList splitTableRow(const QString &line) {
	const QString trimmed = line.trimmed();
	if (trimmed.isEmpty() || !lineHasUnescapedPipe(trimmed)) {
		return {};
	}
	int start = 0;
	int end = trimmed.size();
	if (trimmed.at(start) == QLatin1Char('|')) {
		++start;
	}
	if (end > start && trimmed.at(end - 1) == QLatin1Char('|')) {
		int backslashes = 0;
		for (int p = end - 2; p >= start && trimmed.at(p) == QLatin1Char('\\'); --p) {
			++backslashes;
		}
		if ((backslashes % 2) == 0) {
			--end;
		}
	}
	QStringList cells;
	QString current;
	for (int i = start; i < end; ++i) {
		const QChar ch = trimmed.at(i);
		if (ch == QLatin1Char('\\') && i + 1 < end && trimmed.at(i + 1) == QLatin1Char('|')) {
			current.append(QLatin1Char('|'));
			++i;
			continue;
		}
		if (ch == QLatin1Char('|')) {
			cells.push_back(current.trimmed());
			current.clear();
			if (cells.size() >= 20) {
				break;
			}
			continue;
		}
		current.append(ch);
	}
	cells.push_back(current.trimmed());
	return cells;
}

bool parseTableSeparatorRow(const QString &line, QVector<Qt::Alignment> &outAlignments) {
	const QStringList cells = splitTableRow(line);
	if (cells.isEmpty()) {
		return false;
	}
	outAlignments.clear();
	outAlignments.reserve(cells.size());
	for (const QString &cell : cells) {
		if (cell.isEmpty()) {
			return false;
		}
		int i = 0;
		int n = cell.size();
		bool leftColon = false;
		bool rightColon = false;
		if (cell.at(i) == QLatin1Char(':')) {
			leftColon = true;
			++i;
		}
		if (n > i && cell.at(n - 1) == QLatin1Char(':')) {
			rightColon = true;
			--n;
		}
		if (i >= n) {
			return false;
		}
		for (int p = i; p < n; ++p) {
			if (cell.at(p) != QLatin1Char('-')) {
				return false;
			}
		}
		if (leftColon && rightColon) {
			outAlignments.push_back(Qt::AlignHCenter);
		} else if (rightColon) {
			outAlignments.push_back(Qt::AlignRight);
		} else {
			outAlignments.push_back(Qt::AlignLeft);
		}
	}
	return true;
}

int findClosingMarker(const QString &src, int from, const QString &marker) {
	int pos = from;
	while (pos < src.size()) {
		if (src.at(pos) == QLatin1Char('\\') && pos + 1 < src.size()) {
			pos += 2;
			continue;
		}
		if (src.mid(pos, marker.size()) == marker) {
			return pos;
		}
		++pos;
	}
	return -1;
}

int findClosingWordBoundedMarker(const QString &src, int from, const QString &marker) {
	int pos = from;
	const int mLen = marker.size();
	while (pos <= src.size() - mLen) {
		if (src.at(pos) == QLatin1Char('\\') && pos + 1 < src.size()) {
			pos += 2;
			continue;
		}
		if (src.mid(pos, mLen) == marker) {
			const bool validAfter = (pos + mLen >= src.size()) || !isIdentifierChar(src.at(pos + mLen));
			const bool spaceBefore = (pos > from) && src.at(pos - 1).isSpace();
			if (validAfter && !spaceBefore) {
				return pos;
			}
		}
		++pos;
	}
	return -1;
}

int findSingleClosingChar(const QString &src, int from, QChar markerChar, bool wordBounded) {
	int pos = from;
	while (pos < src.size()) {
		if (src.at(pos) == QLatin1Char('\\') && pos + 1 < src.size()) {
			pos += 2;
			continue;
		}
		if (src.at(pos) == markerChar) {
			if (pos + 1 < src.size() && src.at(pos + 1) == markerChar) {
				pos += 2;
				continue;
			}
			if (wordBounded) {
				const bool validAfter = (pos + 1 >= src.size()) || !isIdentifierChar(src.at(pos + 1));
				if (!validAfter) {
					++pos;
					continue;
				}
			}
			return pos;
		}
		++pos;
	}
	return -1;
}

void appendInlineMarkdown(
		const QString &line,
		QString &outText,
		EntitiesInText &outEntities,
		bool allowUnclosedStreaming) {
	int i = 0;
	const int n = line.size();
	while (i < n) {
		const QChar ch = line.at(i);

		// Backslash escape for ASCII punctuation.
		if (ch == QLatin1Char('\\') && i + 1 < n) {
			const QChar next = line.at(i + 1);
			if (next == QLatin1Char('\\') || next == QLatin1Char('*') ||
				next == QLatin1Char('`') || next == QLatin1Char('~') ||
				next == QLatin1Char('[') || next == QLatin1Char(']') ||
				next == QLatin1Char('_') || next == QLatin1Char('|')) {
				outText.append(next);
				i += 2;
				continue;
			}
		}

		// Inline code: `...`
		if (ch == QLatin1Char('`')) {
			const int close = line.indexOf(QLatin1Char('`'), i + 1);
			if (close > i + 1) {
				const int start = outText.size();
				const QString inner = line.mid(i + 1, close - i - 1);
				outText.append(inner);
				outEntities.push_back(EntityInText(EntityType::Code, start, inner.size()));
				i = close + 1;
				continue;
			}
		}

		// Markdown link: [label](url)
		if (ch == QLatin1Char('[')) {
			const int closeBracket = line.indexOf(QLatin1Char(']'), i + 1);
			if (closeBracket > i + 1 &&
				closeBracket + 1 < n &&
				line.at(closeBracket + 1) == QLatin1Char('(')) {
				const int closeParen = line.indexOf(QLatin1Char(')'), closeBracket + 2);
				if (closeParen > closeBracket + 2) {
					const QString label = line.mid(i + 1, closeBracket - i - 1);
					const QString url = line.mid(closeBracket + 2, closeParen - closeBracket - 2).trimmed();
					if (!label.isEmpty() && isSafeExternalOrInternalUrl(url)) {
						const int start = outText.size();
						appendInlineMarkdown(label, outText, outEntities, false);
						const int len = outText.size() - start;
						if (len > 0) {
							outEntities.push_back(EntityInText(EntityType::CustomUrl, start, len, url));
						}
						i = closeParen + 1;
						continue;
					}
				}
			}
		}

		// Bold: **...** (also tolerate unclosed trailing ** during streaming)
		if (ch == QLatin1Char('*') && i + 1 < n && line.at(i + 1) == QLatin1Char('*')) {
			const int close = findClosingMarker(line, i + 2, QStringLiteral("**"));
			if (close > i + 2) {
				const int start = outText.size();
				appendInlineMarkdown(line.mid(i + 2, close - i - 2), outText, outEntities, false);
				const int len = outText.size() - start;
				if (len > 0) {
					outEntities.push_back(EntityInText(EntityType::Bold, start, len));
				}
				i = close + 2;
				continue;
			}
			if (allowUnclosedStreaming && i + 2 < n) {
				const int start = outText.size();
				appendInlineMarkdown(line.mid(i + 2), outText, outEntities, false);
				const int len = outText.size() - start;
				if (len > 0) {
					outEntities.push_back(EntityInText(EntityType::Bold, start, len));
				}
				break;
			}
		}

		// Bold: __...__ (identifier-boundary safe)
		if (ch == QLatin1Char('_') && i + 1 < n && line.at(i + 1) == QLatin1Char('_')) {
			const bool validBefore = (i == 0) || !isIdentifierChar(line.at(i - 1));
			if (validBefore && i + 2 < n && !line.at(i + 2).isSpace()) {
				const int close = findClosingWordBoundedMarker(line, i + 2, QStringLiteral("__"));
				if (close > i + 2) {
					const int start = outText.size();
					appendInlineMarkdown(line.mid(i + 2, close - i - 2), outText, outEntities, false);
					const int len = outText.size() - start;
					if (len > 0) {
						outEntities.push_back(EntityInText(EntityType::Bold, start, len));
					}
					i = close + 2;
					continue;
				}
			}
		}

		// Strikethrough: ~~...~~
		if (ch == QLatin1Char('~') && i + 1 < n && line.at(i + 1) == QLatin1Char('~')) {
			const int close = findClosingMarker(line, i + 2, QStringLiteral("~~"));
			if (close > i + 2) {
				const int start = outText.size();
				appendInlineMarkdown(line.mid(i + 2, close - i - 2), outText, outEntities, false);
				const int len = outText.size() - start;
				if (len > 0) {
					outEntities.push_back(EntityInText(EntityType::StrikeOut, start, len));
				}
				i = close + 2;
				continue;
			}
		}

		// Italic: *...*
		if (ch == QLatin1Char('*') && !(i + 1 < n && line.at(i + 1) == QLatin1Char('*'))) {
			const int close = findSingleClosingChar(line, i + 1, QLatin1Char('*'), false);
			if (close > i + 1) {
				const QString inner = line.mid(i + 1, close - i - 1);
				if (!inner.startsWith(QLatin1Char(' ')) && !inner.endsWith(QLatin1Char(' '))) {
					const int start = outText.size();
					appendInlineMarkdown(inner, outText, outEntities, false);
					const int len = outText.size() - start;
					if (len > 0) {
						outEntities.push_back(EntityInText(EntityType::Italic, start, len));
					}
					i = close + 1;
					continue;
				}
			}
		}

		// Italic: _..._ (identifier-boundary safe so snake_case names stay literal)
		if (ch == QLatin1Char('_') && !(i + 1 < n && line.at(i + 1) == QLatin1Char('_'))) {
			const bool validBefore = (i == 0) || !isIdentifierChar(line.at(i - 1));
			if (validBefore) {
				const int close = findSingleClosingChar(line, i + 1, QLatin1Char('_'), true);
				if (close > i + 1) {
					const QString inner = line.mid(i + 1, close - i - 1);
					if (!inner.startsWith(QLatin1Char(' ')) && !inner.endsWith(QLatin1Char(' '))) {
						const int start = outText.size();
						appendInlineMarkdown(inner, outText, outEntities, false);
						const int len = outText.size() - start;
						if (len > 0) {
							outEntities.push_back(EntityInText(EntityType::Italic, start, len));
						}
						i = close + 1;
						continue;
					}
				}
			}
		}

		outText.append(ch);
		++i;
	}
}

struct BlockAccumulator {
	ParsedMarkdownBlock result;
	TextWithEntities currentRichSegment;

	void ensureSeparator(bool doubleGap = false) {
		appendParagraphSeparator(result.richText.text, doubleGap);
		appendParagraphSeparator(currentRichSegment.text, doubleGap);
	}

	void appendInlineBlock(const TextWithEntities &block, bool doubleGapBefore = false) {
		ensureSeparator(doubleGapBefore);
		appendWithShift(result.richText, block);
		appendWithShift(currentRichSegment, block);
	}

	void flushRichSegment() {
		if (currentRichSegment.text.trimmed().isEmpty() && currentRichSegment.entities.isEmpty()) {
			currentRichSegment = TextWithEntities{};
			return;
		}
		finalizeRichText(currentRichSegment);
		if (!currentRichSegment.text.isEmpty()) {
			ParsedMarkdownBlock::Segment seg;
			seg.type = ParsedMarkdownBlock::Segment::Type::RichText;
			seg.richText = std::move(currentRichSegment);
			result.segments.push_back(std::move(seg));
		}
		currentRichSegment = TextWithEntities{};
	}
};

} // namespace

ParsedMarkdownBlock CopilotMarkdownParser::parse(const QString &markdown) {
	ParsedMarkdownBlock empty;
	if (markdown.isEmpty()) {
		return empty;
	}

	QString normalized = markdown;
	normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
	normalized.replace(QLatin1Char('\r'), QLatin1Char('\n'));

	const QStringList lines = normalized.split(QLatin1Char('\n'));
	BlockAccumulator acc;

	const int lineCount = lines.size();
	int idx = 0;
	while (idx < lineCount) {
		const QString &line = lines.at(idx);

		// 1. Fenced code block (``` or ~~~, including streaming unclosed fence)
		QChar fenceChar;
		int fenceCount = 0;
		QString lang;
		if (matchFenceOpen(line, fenceChar, fenceCount, lang)) {
			QStringList codeLines;
			++idx;
			while (idx < lineCount && !matchFenceClose(lines.at(idx), fenceChar, fenceCount)) {
				codeLines.push_back(lines.at(idx));
				++idx;
			}
			if (idx < lineCount && matchFenceClose(lines.at(idx), fenceChar, fenceCount)) {
				++idx;
			}
			const QString codeText = codeLines.join(QLatin1Char('\n'));
			if (!codeText.isEmpty()) {
				appendParagraphSeparator(acc.result.richText.text);
				const int start = acc.result.richText.text.size();
				acc.result.richText.text.append(codeText);
				const int len = acc.result.richText.text.size() - start;
				acc.result.richText.entities.push_back(EntityInText(EntityType::Pre, start, len, lang));
				ParsedMarkdownBlock::CodeBlockSpan span{ start, len, lang, codeText };
				acc.result.codeBlocks.push_back(span);

				acc.flushRichSegment();
				ParsedMarkdownBlock::Segment seg;
				seg.type = ParsedMarkdownBlock::Segment::Type::CodeBlock;
				seg.codeBlock = span;
				seg.codeBlock.offset = 0;
				acc.result.segments.push_back(std::move(seg));
			}
			continue;
		}

		// 2. Blockquote lines: > ...
		if (line.trimmed().startsWith(QLatin1Char('>'))) {
			TextWithEntities quoteBlock;
			bool firstQuoteLine = true;
			while (idx < lineCount && lines.at(idx).trimmed().startsWith(QLatin1Char('>'))) {
				QString content = lines.at(idx).trimmed().mid(1);
				if (content.startsWith(QLatin1Char(' '))) {
					content.remove(0, 1);
				}
				if (!firstQuoteLine) {
					quoteBlock.text.append(QLatin1Char('\n'));
				}
				firstQuoteLine = false;
				const bool isLast = (idx + 1 == lineCount);
				appendInlineMarkdown(content, quoteBlock.text, quoteBlock.entities, isLast);
				++idx;
			}
			if (!quoteBlock.text.isEmpty()) {
				quoteBlock.entities.push_back(EntityInText(EntityType::Blockquote, 0, quoteBlock.text.size()));
			}
			acc.appendInlineBlock(quoteBlock);
			continue;
		}

		// 3. Blank line -> preserve paragraph break if previous text is non-empty
		if (line.trimmed().isEmpty()) {
			if (!acc.result.richText.text.isEmpty() && !acc.result.richText.text.endsWith(QStringLiteral("\n\n"))) {
				appendParagraphSeparator(acc.result.richText.text, true);
			}
			if (!acc.currentRichSegment.text.isEmpty() &&
				!acc.currentRichSegment.text.endsWith(QStringLiteral("\n\n"))) {
				appendParagraphSeparator(acc.currentRichSegment.text, true);
			}
			++idx;
			continue;
		}

		// 4. Horizontal rule
		if (matchHorizontalRule(line)) {
			TextWithEntities hrBlock;
			hrBlock.text = QStringLiteral("────────────────");
			acc.appendInlineBlock(hrBlock);
			++idx;
			continue;
		}

		// 5. Markdown heading (# .. ######)
		QString headingText;
		if (matchHeading(line, headingText)) {
			TextWithEntities headingBlock;
			appendInlineMarkdown(headingText, headingBlock.text, headingBlock.entities, (idx + 1 == lineCount));
			if (!headingBlock.text.isEmpty()) {
				headingBlock.entities.push_back(EntityInText(EntityType::Bold, 0, headingBlock.text.size()));
			}
			acc.appendInlineBlock(headingBlock, !acc.result.richText.text.isEmpty());
			++idx;
			continue;
		}

		// 6. GFM Table: header row + delimiter alignment row + 0..N body rows
		if (idx + 1 < lineCount && lineHasUnescapedPipe(line)) {
			const QStringList headerCells = splitTableRow(line);
			QVector<Qt::Alignment> alignments;
			if (!headerCells.isEmpty() &&
				parseTableSeparatorRow(lines.at(idx + 1), alignments) &&
				alignments.size() == headerCells.size()) {
				ParsedMarkdownBlock::MarkdownTableData table;
				table.alignments = alignments;

				const int colCount = headerCells.size();
				const auto parseRow = [colCount, lineCount](const QStringList &cells, int rowLineIdx) {
					QVector<TextWithEntities> row;
					row.reserve(colCount);
					for (int c = 0; c < colCount; ++c) {
						TextWithEntities cellRich;
						if (c < cells.size()) {
							appendInlineMarkdown(
								cells.at(c),
								cellRich.text,
								cellRich.entities,
								(rowLineIdx + 1 == lineCount));
						}
						finalizeRichText(cellRich);
						row.push_back(std::move(cellRich));
					}
					return row;
				};

				table.rows.push_back(parseRow(headerCells, idx));
				idx += 2;
				while (idx < lineCount) {
					const QString &rowLine = lines.at(idx);
					const QString rowTrimmed = rowLine.trimmed();
					QChar unusedChar;
					int unusedCount = 0;
					QString unusedLang;
					if (rowTrimmed.isEmpty() ||
						matchFenceOpen(rowLine, unusedChar, unusedCount, unusedLang) ||
						matchHorizontalRule(rowLine) ||
						rowTrimmed.startsWith(QLatin1Char('>')) ||
						!lineHasUnescapedPipe(rowLine)) {
						break;
					}
					table.rows.push_back(parseRow(splitTableRow(rowLine), idx));
					++idx;
				}

				// Keep flat text fallback in legacy result.richText.
				appendParagraphSeparator(acc.result.richText.text);
				for (int r = 0; r < table.rows.size(); ++r) {
					if (r > 0) {
						acc.result.richText.text.append(QLatin1Char('\n'));
					}
					QStringList cellTexts;
					cellTexts.reserve(table.rows[r].size());
					for (const auto &cell : table.rows[r]) {
						cellTexts.push_back(cell.text);
					}
					acc.result.richText.text.append(cellTexts.join(QStringLiteral(" | ")));
				}

				acc.flushRichSegment();
				ParsedMarkdownBlock::Segment seg;
				seg.type = ParsedMarkdownBlock::Segment::Type::Table;
				seg.table = std::move(table);
				acc.result.segments.push_back(std::move(seg));
				continue;
			}
		}

		// 7. Unordered list (- / * / +)
		int indent = 0;
		QString listBody;
		if (matchUnorderedList(line, indent, listBody)) {
			TextWithEntities itemBlock;
			if (indent > 0) {
				itemBlock.text.append(QString(std::min(indent, 6), QLatin1Char(' ')));
			}
			itemBlock.text.append(QStringLiteral("• "));
			appendInlineMarkdown(listBody, itemBlock.text, itemBlock.entities, (idx + 1 == lineCount));
			acc.appendInlineBlock(itemBlock);
			++idx;
			continue;
		}

		// 8. Ordered list (1. / 1) )
		QString ordPrefix;
		if (matchOrderedList(line, indent, ordPrefix, listBody)) {
			TextWithEntities itemBlock;
			if (indent > 0) {
				itemBlock.text.append(QString(std::min(indent, 6), QLatin1Char(' ')));
			}
			itemBlock.text.append(ordPrefix);
			appendInlineMarkdown(listBody, itemBlock.text, itemBlock.entities, (idx + 1 == lineCount));
			acc.appendInlineBlock(itemBlock);
			++idx;
			continue;
		}

		// 9. Ordinary text line
		TextWithEntities paraBlock;
		appendInlineMarkdown(line, paraBlock.text, paraBlock.entities, (idx + 1 == lineCount));
		acc.appendInlineBlock(paraBlock);
		++idx;
	}

	acc.flushRichSegment();
	finalizeRichText(acc.result.richText);
	return acc.result;
}

} // namespace OpenMeeting
