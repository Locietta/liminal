# Provider Context Semantics

This note records the provider behavior that constrains Liminal's context model. It is not an implementation plan or roadmap.

## Durable rules

- Instructions, user input, and model output are different semantic data even when a provider represents them with message roles.
- Model-generated conversation is always assistant output. Replaying it as a system or developer instruction would incorrectly increase its authority.
- Tool results, retrieved documents, summaries, and other untrusted context remain data; relevant content does not become an instruction.
- Provider-native reasoning and compaction items retain provider provenance and are replayed only through a compatible provider adapter.
- Liminal owns semantic session state. Provider-shaped request history is a derived projection for one model call.

## OpenAI Responses

OpenAI assigns application rules to `developer`, end-user input to `user`, and model-generated messages to `assistant`. Liminal therefore lowers application and trusted project instructions as developer guidance while preserving generated turns as assistant output.

The Responses API also accepts top-level `instructions`. Those instructions apply to the current response and are not automatically carried forward with `previous_response_id`. Liminal currently constructs requests from explicit, manually managed input items, so explicit developer messages keep the instruction prefix visible and reproducible. Switching to top-level `instructions` should require evidence that the resulting semantics remain equivalent.

Tool calls, tool results, reasoning items, and compaction items use their typed Responses API forms rather than being disguised as chat messages. Hosted web access uses the provider's Responses tool, and provider-owned state needed for stateless replay remains provider-tagged semantic data.

## Anthropic Messages

Anthropic conversation messages use `user` and `assistant`; governing instructions belong in the top-level `system` field. Anthropic has no generally portable equivalent of OpenAI's developer role, so Liminal serializes its ordered runtime, application, and project instructions into the system prompt while preserving generated history as assistant messages.

Provider-native hosted tools use their typed server-tool forms. Any provider-owned state required to replay those results remains provider-tagged and is omitted by incompatible adapters rather than translated into invented portable semantics.

Provider-specific features such as model-dependent mid-conversation system messages must not define Liminal's base session semantics.

## Context guidance

Prompt quality is not instruction volume. Keep the stable runtime prompt small, put tool behavior in tool schemas, load specialized workflows on demand, and keep repository instructions focused on constraints that cannot be inferred from code and tests. Avoid repeating the same rule across the runtime prompt, project guidance, skills, and tool descriptions.

## References

Accessed 2026-08-06.

- OpenAI, [Message roles and instruction following](https://developers.openai.com/api/docs/guides/prompt-engineering#message-roles-and-instruction-following)
- OpenAI, [Migrate to the Responses API: map messages to items](https://developers.openai.com/api/docs/guides/migrate-to-responses#2-map-messages-to-items)
- OpenAI, [Create a response](https://developers.openai.com/api/reference/resources/responses/methods/create)
- OpenAI, [Continue after client-owned function calls](https://developers.openai.com/api/docs/guides/tools-programmatic-tool-calling#continue-after-client-owned-function-calls)
- OpenAI, [Model Spec: chain of command](https://model-spec.openai.com/2025-02-12.html#chain_of_command)
- Anthropic, [The new rules of context engineering for Claude 5-generation models](https://claude.com/blog/the-new-rules-of-context-engineering-for-claude-5-generation-models)
- Anthropic, [Create a message](https://platform.claude.com/docs/en/api/messages/create)
- Anthropic, [Working with messages](https://platform.claude.com/docs/en/build-with-claude/working-with-messages)
