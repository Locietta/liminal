# Providers and models

Liminal routes every model through a named provider instance. A provider owns its display name,
base URL, authentication, wire API, explicit models, and optional model discovery. The supported
wire APIs are `openai-responses` and `anthropic-messages`; any number of custom providers may use
either API.

## Provider configuration

Provider configuration is read from:

- Windows: `%APPDATA%\liminal\providers.json`
- Linux: `$XDG_CONFIG_HOME/liminal/providers.json`, or `~/.config/liminal/providers.json`

`LIMINAL_PROVIDERS_FILE` overrides that path. The file is reloaded at startup and whenever
`/model` runs.

The following example defines two independent custom providers:

```json
{
  "providers": {
    "openai-compatible": {
      "name": "My Responses Gateway",
      "api": "openai-responses",
      "base_url": "https://gateway.example.com/v1",
      "api_key": "$MY_OPENAI_KEY",
      "discover_models": true,
      "models": [
        {
          "id": "agent-model",
          "name": "Agent Model",
          "context_window": 128000,
          "max_output_tokens": 8192,
          "context_safety_margin_tokens": 4096,
          "reasoning_efforts": ["low", "medium", "high"],
          "default_reasoning_effort": "medium"
        }
      ]
    },
    "anthropic-compatible": {
      "name": "My Messages Gateway",
      "api": "anthropic-messages",
      "base_url": "https://messages.example.com",
      "api_key": "$MY_ANTHROPIC_KEY",
      "models": [
        {
          "id": "claude-model",
          "reasoning_efforts": ["low", "medium", "high"]
        }
      ]
    }
  }
}
```

An `api_key` beginning with `$` names an environment variable. A literal key is also accepted, but
is stored as plaintext in `providers.json`. OpenAI-compatible providers send the key as a bearer
token; Anthropic-compatible providers send it as `x-api-key`.

Provider IDs, such as `openai-compatible`, are stable local identifiers. They distinguish multiple
providers using the same wire API and qualify model names when two providers expose the same model
ID.

## Explicit models and discovery

Explicit models are authoritative and remain available even if a provider has no Models API.
Remote discovery is disabled by default. Set `discover_models` to `true` for a provider to add IDs
returned by its `/models` endpoint. When a discovered ID matches an explicit entry, the explicit
name and reasoning metadata win.

Discovery is best-effort and provider-scoped. A discovery failure produces a warning without
removing explicit models or models discovered from other providers.

Reasoning metadata must be configured explicitly because standard Models APIs do not report the
supported effort values. OpenAI Responses providers receive `reasoning.effort`; Anthropic Messages
providers receive adaptive thinking plus `output_config.effort`.

Context capabilities are likewise explicit because standard model discovery does not reliably
report them. `context_window` is the total token window, `max_output_tokens` is both the request
limit and reserved output budget, and `context_safety_margin_tokens` leaves extra headroom for
token estimation and provider-owned request material. Discovered models without an explicit
`context_window` remain selectable, but automatic context budgeting is disabled for them.

## Codex subscription

The built-in `codex` provider uses ChatGPT Plus/Pro device-code OAuth:

```text
liminal login codex
```

Follow the printed URL and enter the displayed code. Liminal stores access and refresh tokens in
`auth.json` and refreshes them before requests. The default authentication file is:

- Windows: `%APPDATA%\liminal\auth.json`
- Linux: `$XDG_CONFIG_HOME/liminal/auth.json`, or `~/.config/liminal/auth.json`

`LIMINAL_AUTH_FILE` overrides that path. On Linux, Liminal restricts the file to the current user.

After login, Liminal exposes its bundled Codex model catalog without requiring remote discovery.
The initial catalog includes GPT-5.6 Sol, Terra, Luna, and GPT-5.5. The built-in API URL, wire API,
and authentication strategy cannot be overridden.

A `codex` entry in `providers.json` may customize its display name, enable discovery, override
bundled model metadata, or add account-specific models:

```json
{
  "providers": {
    "codex": {
      "name": "My Codex Subscription",
      "discover_models": true,
      "models": [
        {
          "id": "gpt-5.6-sol",
          "name": "Customized Sol",
          "reasoning_efforts": ["low", "medium", "high"],
          "default_reasoning_effort": "medium"
        },
        {
          "id": "account-specific-model"
        }
      ]
    }
  }
}
```

Codex discovery adds models returned by the subscription's Models API. Bundled and explicitly
configured metadata remain authoritative.

## Selecting a model

The command surface is model-centric:

```text
/model
/model gpt-5.6-sol
/model codex/gpt-5.6-sol@high
```

`/model` reloads provider configuration, performs opted-in discovery, and lists the combined
catalog. `/model <id>` selects an unambiguous model. Use `<provider>/<id>` when two providers expose
the same ID. Append `@<effort>` to select a declared reasoning effort; `@off` clears an explicit
effort.

Model changes apply to subsequent turns while retaining provider-neutral conversation history.
There is no separate provider-selection state.

`LIMINAL_MODEL` selects the startup model using the same selector syntax. When it is unset, Liminal
uses the first available catalog entry.
