#pragma once

#include <vector>

#include <liminal/session/session.h>
#include <liminal/tools/tools.h>
#include <liminal/tui/transcript.h>

namespace liminal::tui {

std::vector<Block> project_transcript(const session::Session &session, const ToolSet &tools);
Result<std::vector<Block>> project_transcript_at(const session::Session &session, session::ConversationCheckpointId checkpoint,
                                                 const ToolSet &tools);

} // namespace liminal::tui
