#pragma once

#include "ui/text/text_entity.h"

#include <QtCore/QString>
#include <QtCore/QVector>
#include <QtCore/Qt>

namespace OpenMeeting {

struct ParsedMarkdownBlock {
	struct CodeBlockSpan {
		int offset = 0;
		int length = 0;
		QString language;
		QString rawCode;
	};

	struct MarkdownTableData {
		QVector<QVector<TextWithEntities>> rows;
		QVector<Qt::Alignment> alignments;
	};

	struct Segment {
		enum class Type {
			RichText = 0,
			CodeBlock = 1,
			Table = 2,
		};
		Type type = Type::RichText;
		TextWithEntities richText;
		CodeBlockSpan codeBlock;
		MarkdownTableData table;
	};

	TextWithEntities richText;
	QVector<CodeBlockSpan> codeBlocks;
	QVector<Segment> segments;
};

class CopilotMarkdownParser final {
public:
	// Parses LLM Markdown (including unclosed streaming fences/bold markers) into
	// UTF-16-aligned tdesktop TextWithEntities ready for Ui::Text::String.
	[[nodiscard]] static ParsedMarkdownBlock parse(const QString &markdown);
};

} // namespace OpenMeeting
