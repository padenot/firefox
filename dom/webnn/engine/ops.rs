/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! Per-operator FFI. One `webnn_graph_builder_<op>` function per
//! MLGraphBuilder operator is generated from WebNN.webidl by
//! dom/webnn/codegen/webnn_codegen.py, along with `#[repr(C)]` structs mirroring
//! the IDL arguments and dictionaries. Each converts its arguments with `Conv`
//! into the types rustnn's `<op>_with_options` method takes, so the generator
//! does not need to know them.

use std::str::FromStr;

use rustnn::mlcontext::MLOperand;
use rustnn::mlgraphbuilder::MLGraphBuilder;
use rustnn::operator_options::*;

use crate::{Error, WebnnGraphBuilder, WebnnStatus, ffi_call, write_outputs};

pub trait Conv<T> {
    fn conv(&self) -> Result<T, Error>;
}

fn conv<T, F: Conv<T>>(value: &F) -> Result<T, Error> {
    value.conv()
}

macro_rules! identity_conv {
    ($($t:ty),*) => {
        $(impl Conv<$t> for $t {
            fn conv(&self) -> Result<$t, Error> {
                Ok(*self)
            }
        })*
    };
}

identity_conv!(bool, u32, i32, f32, f64);

impl Conv<Option<bool>> for bool {
    fn conv(&self) -> Result<Option<bool>, Error> {
        Ok(Some(*self))
    }
}

impl Conv<MLDimension> for u32 {
    fn conv(&self) -> Result<MLDimension, Error> {
        Ok(MLDimension::Static(*self))
    }
}

fn number(value: f64) -> Result<serde_json::Value, Error> {
    serde_json::Number::from_f64(value)
        .map(serde_json::Value::Number)
        .ok_or_else(|| Error::InvalidArgument(format!("unsupported number: {value}")))
}

impl Conv<Option<serde_json::Value>> for f64 {
    fn conv(&self) -> Result<Option<serde_json::Value>, Error> {
        Ok(Some(number(*self)?))
    }
}

impl Conv<Option<serde_json::Value>> for WebnnOptionalF64 {
    fn conv(&self) -> Result<Option<serde_json::Value>, Error> {
        self.present.then(|| number(self.value)).transpose()
    }
}

impl Conv<MLOperand> for WebnnOperandId {
    fn conv(&self) -> Result<MLOperand, Error> {
        Ok(MLOperand::from(self.0))
    }
}

impl Conv<Option<MLOperand>> for WebnnOperandId {
    fn conv(&self) -> Result<Option<MLOperand>, Error> {
        Ok(Some(MLOperand::from(self.0)))
    }
}

impl Conv<Option<OperandIndex>> for WebnnOptionalOperand {
    fn conv(&self) -> Result<Option<OperandIndex>, Error> {
        Ok(self.present.then_some(self.id.0))
    }
}

impl Conv<String> for WebnnStr {
    fn conv(&self) -> Result<String, Error> {
        let bytes = unsafe { slice(self.data, self.len)? };
        String::from_utf8(bytes.to_vec())
            .map_err(|_| Error::InvalidArgument("invalid UTF-8".to_string()))
    }
}

unsafe fn slice<'a, T>(data: *const T, len: usize) -> Result<&'a [T], Error> {
    if len == 0 {
        Ok(&[])
    } else if data.is_null() {
        Err(Error::InvalidArgument("null slice".to_string()))
    } else {
        Ok(unsafe { std::slice::from_raw_parts(data, len) })
    }
}

impl<E: Conv<T>, T> Conv<Vec<T>> for Slice<E> {
    fn conv(&self) -> Result<Vec<T>, Error> {
        unsafe { slice(self.data, self.len)? }.iter().map(Conv::conv).collect()
    }
}

impl<E: Conv<T>, T> Conv<Option<Vec<T>>> for OptionalSlice<E> {
    fn conv(&self) -> Result<Option<Vec<T>>, Error> {
        if !self.present {
            return Ok(None);
        }
        let values = unsafe { slice(self.data, self.len)? };
        Ok(Some(values.iter().map(Conv::conv).collect::<Result<_, _>>()?))
    }
}

/// rustnn uses an empty Vec for some absent IDL sequences.
impl<E: Conv<T>, T> Conv<Vec<T>> for OptionalSlice<E> {
    fn conv(&self) -> Result<Vec<T>, Error> {
        Ok(Conv::<Option<Vec<T>>>::conv(self)?.unwrap_or_default())
    }
}

/// `(unsigned long or sequence<unsigned long>)` for split().
pub enum Splits {
    Count(u32),
    Sizes(Vec<u32>),
}

impl Conv<Splits> for WebnnUnsignedLongOrUnsignedLongSequence {
    fn conv(&self) -> Result<Splits, Error> {
        match self.tag {
            0 => Ok(Splits::Count(self.unsigned_long)),
            _ => Ok(Splits::Sizes(conv(&self.unsigned_long_sequence)?)),
        }
    }
}

trait SplitUnion {
    fn split_union_with_options(
        &mut self,
        input: MLOperand,
        splits: Splits,
        options: MLSplitOptions,
    ) -> rustnn::mlgraphbuilder::Result<Vec<MLOperand>>;
}

impl SplitUnion for MLGraphBuilder<'_, '_> {
    fn split_union_with_options(
        &mut self,
        input: MLOperand,
        splits: Splits,
        options: MLSplitOptions,
    ) -> rustnn::mlgraphbuilder::Result<Vec<MLOperand>> {
        match splits {
            Splits::Count(count) => self.split_equal_with_options(input, count, options),
            Splits::Sizes(sizes) => self.split_with_options(input, &sizes, options),
        }
    }
}

pub struct Outputs(Vec<MLOperand>);

impl From<MLOperand> for Outputs {
    fn from(operand: MLOperand) -> Self {
        Outputs(vec![operand])
    }
}

impl From<Vec<MLOperand>> for Outputs {
    fn from(operands: Vec<MLOperand>) -> Self {
        Outputs(operands)
    }
}

/// Records an operation with `f`, and writes the ids of its outputs.
unsafe fn add_operation<A>(
    builder: *mut WebnnGraphBuilder,
    args: *const A,
    outputs: *mut u32,
    outputs_capacity: usize,
    outputs_len: *mut usize,
    f: impl FnOnce(&mut MLGraphBuilder<'static, 'static>, &A) -> Result<Outputs, Error>,
) -> WebnnStatus {
    ffi_call(|| {
        let builder = unsafe { &mut *builder };
        let result = f(&mut builder.0, unsafe { &*args })?;
        write_outputs(result.0, outputs, outputs_capacity, outputs_len)
    })
}

include!(mozbuild::objdir_path!("dom/webnn/engine/webnn_ops.rs"));
