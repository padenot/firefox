# WebNN operator validators

Each MLGraphBuilder operator calls a hand-written validator, declared in the
generated `mozilla/webnn/WebNNValidation.h`:

```cpp
namespace mozilla::webnn {
struct OperandDescriptor {
  dom::MLOperandDataType mDataType;
  nsTArray<uint32_t> mShape;
};
bool ValidateConv2d(const dom::MLOperand& aInput, const dom::MLOperand& aFilter,
                    const dom::MLConv2dOptions& aOptions,
                    nsTArray<OperandDescriptor>& aOutputs, ErrorResult& aRv);
}
```

- Name: `Validate` + the IDL method name with its first letter uppercased
  (`ValidateGatherND`, `ValidateIsNaN`, `ValidateLstmCell`, `ValidateWhere`).
- Parameters: the IDL parameters in order, named `a` + the IDL name with its
  first letter uppercased, then `aOutputs` and `aRv`.
- On success, append one descriptor per output to `aOutputs`, in order, and
  return true. Only split, gru, lstm and lstmCell return several outputs
  (gruCell returns one). On failure, throw on `aRv` (TypeError per spec) and
  return false.
- The caller has already checked the builder state and that every operand,
  including those in options and sequences, belongs to the builder.

Type mapping, as the bindings pass the arguments:

| IDL | C++ parameter |
|---|---|
| `MLOperand` | `const dom::MLOperand&` |
| `sequence<MLOperand>` | `const dom::Sequence<OwningNonNull<dom::MLOperand>>&` |
| `unsigned long` | `uint32_t` |
| `sequence<unsigned long>` | `const dom::Sequence<uint32_t>&` |
| enum `MLXxx` | `dom::MLXxx` |
| dictionary `MLXxxOptions` | `const dom::MLXxxOptions&` |
| `([EnforceRange] unsigned long or sequence<[EnforceRange] unsigned long>)` (split) | `const dom::RangeEnforcedUnsignedLongOrRangeEnforcedUnsignedLongSequence&`, with `IsRangeEnforcedUnsignedLong()` / `GetAsRangeEnforcedUnsignedLong()` and `IsRangeEnforcedUnsignedLongSequence()` / `GetAsRangeEnforcedUnsignedLongSequence()` |

Dictionary members are `m` + the IDL name with its first letter uppercased:

| IDL member | C++ member |
|---|---|
| with a default value (`groups = 1`, `layout = "nchw"`, `label = ""`) | the plain type (`uint32_t`, `dom::MLInputOperandLayout`, `nsString`) |
| `MLOperand` (`bias`, `scale`, `c`, ...) | `dom::Optional<OwningNonNull<dom::MLOperand>>` |
| `sequence<T>` without default (`padding`, `axes`, `activations`, ...) | `dom::Optional<dom::Sequence<T>>` |
| `MLNumber` without default (clamp `minValue`, `maxValue`) | `dom::Optional<double>` |
| `long` (triangular `diagonal`) | `int32_t` |
| `sequence<float>` (resample2d `scales`) | `dom::Optional<dom::Sequence<float>>` |

`MLNumber` is `unrestricted double`: our bindings have no bigint.

Validators that are not written yet are NotSupportedError stubs in
`dom/webnn/validation/Unimplemented.cpp`. To implement one, add it to another
file in `dom/webnn/validation/` (and to `UNIFIED_SOURCES` in
`dom/webnn/moz.build`), then delete its stub. Shared helpers, such as
`BroadcastShapes`, live in `dom/webnn/validation/ValidationUtils.h`.
