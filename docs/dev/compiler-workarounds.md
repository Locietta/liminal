# Compiler workarounds

Last checked on 2026-09-05 with conda-forge GCC 16.2.0 build 4 on Windows
(`x86_64-w64-mingw32`) and Linux (`x86_64-conda-linux-gnu`).

## Outcome accessor return types

`lighter/async/vocab/outcome.h` uses explicit dependent `member_ref_t<I, Self>`
return types for accessors with preconditions. Using `decltype(auto)` with an
enforced contract on an explicit-object member function causes an ICE in
`check_noexcept_r` at `cp/except.cc:1095`.

To recheck, restore `decltype(auto)` on the three accessors and compile
`lighter/tests/test-outcome/main.cpp`, which checks their cv/ref return types.

## HybridVector counted-range contracts

`lighter/utils/small_vector.h` checks `first != nullptr` inside both
`counted_range` overloads with `contract_assert`. Expressing this as a
precondition causes an ICE in `fold_convert_loc` at `fold-const.cc:2800`, during
the GIMPLE `dom` pass, after inlining into non-trivial element insertion paths.

To recheck, replace each body assertion with `pre(first != nullptr)` and compile
`lighter/tests/test-relocation/main.cpp`.

Both failures reproduce on Windows and Linux at `-O2` and `-O3` with
`-fcontract-evaluation-semantic=enforce`. Recheck with contracts enforced;
release builds use `ignore`, which hides both failures.
