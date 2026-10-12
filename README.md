# Liminal

Yet another agentic CLI. Built with performance and simplicity in mind.

**Liminal** is designed to be a lightweight, efficient tool for managing tasks and automating workflows.

![](https://files.seeusercontent.com/2026/08/18/zKl3/20260818171903717.png)

User guides:

- [Interactive TUI](docs/user/tui.md)
- [Providers and models](docs/user/providers-and-models.md)

## Development

The `lighter` runtime (coroutines, HTTP, codecs) and the `lilac` syntax-highlighting lexers are xmake packages from [Locietta/lighter](https://github.com/Locietta/lighter) and [Locietta/lilac](https://github.com/Locietta/lilac), pinned by the recipes in the `xmake` submodule. To build against a local checkout, set `LIGHTER_SOURCE_DIR` or `LILAC_SOURCE_DIR` to it and run `pixi run xmake require -f -y lighter` (or `lilac`).
