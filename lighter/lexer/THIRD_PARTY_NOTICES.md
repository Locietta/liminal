# Third-Party Notices

Liminal includes source-derived lexer implementations. The Liminal-specific
interfaces, refactoring, and modifications are licensed under the repository's
GNU AGPL v3 license. The upstream portions remain subject to the notices below.

## Notepad4

The lexers under `lighter/lexer/language/` derive classification behavior and language data from the following files in Notepad4
revision `eee400c824b30e0aa41ef06a18ce22cf69b5cbb0`:

- C/C++: `scintilla/lexers/LexCPP.cxx`, `src/EditLexers/stlCPP.cpp`, and `src/EditLexers/stlResource.cpp`.
- Rust: `scintilla/lexers/LexRust.cxx` and `src/EditLexers/stlRust.cpp`.
- Python: `scintilla/lexers/LexPython.cxx` and `src/EditLexers/stlPython.cpp`.
- JavaScript/TypeScript: `scintilla/lexers/LexJavaScript.cxx`, `src/EditLexers/stlJavaScript.cpp`, and
  `src/EditLexers/stlTypeScript.cpp`.
- Go: `scintilla/lexers/LexGo.cxx` and `src/EditLexers/stlGo.cpp`.
- Shell: `scintilla/lexers/LexBash.cxx` and `src/EditLexers/stlBash.cpp`.
- Structured data and configuration: `scintilla/lexers/LexJSON.cxx`, `LexTOML.cxx`, `LexYAML.cxx`, `LexConfig.cxx`, `LexProps.cxx`,
  `LexCSV.cxx`, and `LexDiff.cxx`; `src/EditLexers/stlJSON.cpp`, `stlTOML.cpp`, `stlYAML.cpp`, and the corresponding entries in
  `stlDefault.cpp`.
- Markup: `scintilla/lexers/LexHTML.cxx` and `LexMarkdown.cxx`; `src/EditLexers/stlHTML.cpp`, `stlXML.cpp`, and `stlMarkdown.cpp`.
- Brace-oriented languages: `scintilla/lexers/LexJava.cxx`, `LexCSharp.cxx`, `LexD.cxx`, `LexDart.cxx`, `LexCangjie.cxx`,
  `LexGroovy.cxx`, `LexHaxe.cxx`, `LexKotlin.cxx`, `LexScala.cxx`, `LexSwift.cxx`, `LexZig.cxx`, and `LexAsymptote.cxx`; the matching
  `src/EditLexers/stl*.cpp` language data, including `stlGradle.cpp`.
- CSS: `scintilla/lexers/LexCSS.cxx` and `src/EditLexers/stlCSS.cpp`.
- SQL: `scintilla/lexers/LexSQL.cxx` and `src/EditLexers/stlSQL.cpp`.
- Build and installer scripts: `scintilla/lexers/LexBatch.cxx`, `LexCMake.cxx`, `LexGN.cxx`, `LexMake.cxx`, `LexJam.cxx`,
  `LexInno.cxx`, and `LexNsis.cxx`; the matching `src/EditLexers/stl*.cpp` language data.
- Assembly, intermediate representations, and hardware descriptions: `scintilla/lexers/LexAsm.cxx`, `LexCIL.cxx`, `LexLLVM.cxx`,
  `LexSmali.cxx`, `LexWASM.cxx`, `LexVerilog.cxx`, `LexVHDL.cxx`, and `LexWinHex.cxx`; the matching `src/EditLexers/stl*.cpp`
  language data.
- Functional languages: `scintilla/lexers/LexLisp.cxx`, `LexHaskell.cxx`, `LexOCaml.cxx`, `LexFSharp.cxx`, `LexErlang.cxx`, and
  `LexElixir.cxx`; the matching `src/EditLexers/stl*.cpp` language data.
- Dynamic scripting languages: `scintilla/lexers/LexAutoHotkey.cxx`, `LexAutoIt3.cxx`, `LexAviSynth.cxx`, `LexAwk.cxx`,
  `LexCoffeeScript.cxx`, `LexJulia.cxx`, `LexLua.cxx`, `LexMathematica.cxx`, `LexMatlab.cxx`, `LexNim.cxx`, `LexPerl.cxx`, `LexPHP.cxx`,
  `LexPowerShell.cxx`, `LexR.cxx`, `LexRebol.cxx`, `LexRuby.cxx`, `LexTcl.cxx`, and `LexVim.cxx`; the matching
  `src/EditLexers/stl*.cpp` language data.
- Scientific and legacy languages: `scintilla/lexers/LexAPDL.cxx`, `LexFortran.cxx`, `LexPascal.cxx`, `LexPowerBuilder.cxx`,
  `LexSAS.cxx`, and `LexVB.cxx`; the matching `src/EditLexers/stl*.cpp` language data, including the ABAQUS and VBScript identities.

Notepad4 Copyright © 2011-2026 Zufu Liu and All contributors.
matepath Copyright © 2011-2026 Zufu Liu and All contributors.
Notepad2-mod Copyright © 2010-2017 XhmikosR and All contributors.
Notepad2 Copyright © 2004-2012 Florian Balmer.
metapath Copyright © 1996-2012 Florian Balmer.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of Florian Balmer nor the names of its contributors may be
   used to endorse or promote products derived from this software without
   specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

## Lexilla and Scintilla

Copyright 1998-2026 by Neil Hodgson <neilh@scintilla.org>

All Rights Reserved

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted, provided that
the above copyright notice appear in all copies and that both that copyright
notice and this permission notice appear in supporting documentation.

NEIL HODGSON DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL NEIL
HODGSON BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY
DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
