/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! C API of the WebNN engine, on top of rustnn's Rust API.
//!
//! Handles are opaque and released with their destroy function. A context
//! must outlive the builders, graphs and tensors created from it. Operands are
//! referred to by the ids rustnn gives them. The per-operator functions are
//! generated from WebNN.webidl, see ops.rs.

#![allow(clippy::missing_safety_doc)]

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::ffi::{CStr, CString, c_char, c_void};
use std::panic::{AssertUnwindSafe, catch_unwind};
use std::slice;

use rustnn::mlcontext::{
    MLContext, MLContextOptions, MLGraph, MLNamedOperands, MLNamedTensors, MLOperand,
    MLOperandDescriptor, MLPowerPreference, MLTensor, MLTensorDescriptor,
};
use rustnn::mlgraphbuilder::MLGraphBuilder;
use rustnn::operator_enums::MLOperandDataType;

pub mod ops;

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum WebnnStatus {
    Success = 0,
    InvalidArgument = 1,
    Error = 2,
    Panic = 3,
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub enum WebnnDataType {
    Float32,
    Float16,
    Int32,
    Uint32,
    Int64,
    Uint64,
    Int8,
    Uint8,
}

impl From<WebnnDataType> for MLOperandDataType {
    fn from(value: WebnnDataType) -> Self {
        match value {
            WebnnDataType::Float32 => Self::Float32,
            WebnnDataType::Float16 => Self::Float16,
            WebnnDataType::Int32 => Self::Int32,
            WebnnDataType::Uint32 => Self::Uint32,
            WebnnDataType::Int64 => Self::Int64,
            WebnnDataType::Uint64 => Self::Uint64,
            WebnnDataType::Int8 => Self::Int8,
            WebnnDataType::Uint8 => Self::Uint8,
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub enum WebnnPowerPreference {
    Default,
    HighPerformance,
    LowPower,
}

impl From<WebnnPowerPreference> for MLPowerPreference {
    fn from(value: WebnnPowerPreference) -> Self {
        match value {
            WebnnPowerPreference::Default => Self::Default,
            WebnnPowerPreference::HighPerformance => Self::HighPerformance,
            WebnnPowerPreference::LowPower => Self::LowPower,
        }
    }
}

pub struct WebnnContext(MLContext<'static>);
pub struct WebnnGraphBuilder(MLGraphBuilder<'static, 'static>);
pub struct WebnnGraph(MLGraph<'static>);
pub struct WebnnTensor(MLTensor);

/// A data type and a shape.
#[repr(C)]
pub struct WebnnOperandDescriptor {
    pub data_type: WebnnDataType,
    pub shape: *const u32,
    pub shape_len: usize,
}

#[repr(C)]
pub struct WebnnNamedOperand {
    /// Null-terminated UTF-8.
    pub name: *const c_char,
    pub operand: u32,
}

#[repr(C)]
pub struct WebnnNamedTensor {
    /// Null-terminated UTF-8.
    pub name: *const c_char,
    pub tensor: *const WebnnTensor,
}

#[derive(Debug)]
pub enum Error {
    InvalidArgument(String),
    Unsupported(&'static str),
    Engine(String),
}

impl<E: std::fmt::Display> From<E> for Error {
    fn from(error: E) -> Self {
        Error::Engine(error.to_string())
    }
}

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

fn set_error(message: String) {
    let message = CString::new(message.replace('\0', "\\0")).unwrap_or_default();
    LAST_ERROR.with(|slot| *slot.borrow_mut() = message);
}

/// Runs `f`, catching panics and recording errors for `webnn_last_error_message`.
fn ffi_call(f: impl FnOnce() -> Result<(), Error>) -> WebnnStatus {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => WebnnStatus::Success,
        Ok(Err(Error::InvalidArgument(message))) => {
            set_error(message);
            WebnnStatus::InvalidArgument
        }
        Ok(Err(Error::Unsupported(value))) => {
            set_error(format!("unsupported value: {value}"));
            WebnnStatus::InvalidArgument
        }
        Ok(Err(Error::Engine(message))) => {
            set_error(message);
            WebnnStatus::Error
        }
        Err(_) => {
            set_error("panic in the WebNN engine".to_string());
            WebnnStatus::Panic
        }
    }
}

unsafe fn input_slice<'a, T>(data: *const T, len: usize) -> Result<&'a [T], Error> {
    if len == 0 {
        Ok(&[])
    } else if data.is_null() {
        Err(Error::InvalidArgument("null slice".to_string()))
    } else {
        Ok(unsafe { slice::from_raw_parts(data, len) })
    }
}

unsafe fn input_str<'a>(value: *const c_char) -> Result<&'a str, Error> {
    if value.is_null() {
        return Err(Error::InvalidArgument("null string".to_string()));
    }
    unsafe { CStr::from_ptr(value) }
        .to_str()
        .map_err(|_| Error::InvalidArgument("invalid UTF-8".to_string()))
}

unsafe fn descriptor(desc: &WebnnOperandDescriptor) -> Result<MLOperandDescriptor, Error> {
    let shape = unsafe { input_slice(desc.shape, desc.shape_len)? };
    Ok(MLOperandDescriptor::new(
        desc.data_type.into(),
        shape.iter().map(|&d| d as u64).collect(),
    ))
}

unsafe fn destroy<T>(handle: *mut T) {
    if !handle.is_null() {
        drop(unsafe { Box::from_raw(handle) });
    }
}

/// The message of the last error on this thread, valid until the next error.
#[unsafe(no_mangle)]
pub extern "C" fn webnn_last_error_message() -> *const c_char {
    LAST_ERROR.with(|slot| slot.borrow().as_ptr())
}

/// Sets the directory where the backends write temporary files.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn webnn_set_temp_directory(path: *const c_char) -> WebnnStatus {
    ffi_call(|| {
        let path = unsafe { input_str(path)? };
        rustnn::temp_dir::set_temp_dir(Some(path.into()));
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn webnn_context_create(
    power_preference: WebnnPowerPreference,
    accelerated: bool,
    context: *mut *mut WebnnContext,
) -> WebnnStatus {
    ffi_call(|| {
        let options = MLContextOptions::new(power_preference.into(), accelerated);
        let created = MLContext::create(&options)?;
        unsafe { *context = Box::into_raw(Box::new(WebnnContext(created))) };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn webnn_context_destroy(context: *mut WebnnContext) {
    unsafe { destroy(context) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn webnn_context_create_tensor(
    context: *mut WebnnContext,
    desc: *const WebnnOperandDescriptor,
    readable: bool,
    writable: bool,
    tensor: *mut *mut WebnnTensor,
) -> WebnnStatus {
    ffi_call(|| {
        let context = unsafe { &mut *context };
        let desc = unsafe { descriptor(&*desc)? };
        let mut tensor_desc = MLTensorDescriptor::from_operand_descriptor(&desc);
        tensor_desc.set_readable(readable);
        tensor_desc.set_writable(writable);
        let created = context.0.create_tensor(&tensor_desc)?;
        unsafe { *tensor = Box::into_raw(Box::new(WebnnTensor(created))) };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn webnn_tensor_destroy(tensor: *mut WebnnTensor) {
    unsafe { destroy(tensor) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn webnn_context_write_tensor(
    context: *mut WebnnContext,
    tensor: *const WebnnTensor,
    data: *const c_void,
    len: usize,
) -> WebnnStatus {
    ffi_call(|| {
        let context = unsafe { &mut *context };
        let data = unsafe { input_slice(data.cast::<u8>(), len)? };
        context.0.write_tensor(unsafe { &(*tensor).0 }, data)?;
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn webnn_context_read_tensor(
    context: *mut WebnnContext,
    tensor: *const WebnnTensor,
    data: *mut c_void,
    len: usize,
) -> WebnnStatus {
    ffi_call(|| {
        let context = unsafe { &mut *context };
        let data: &mut [u8] = if len == 0 {
            &mut []
        } else {
            unsafe { slice::from_raw_parts_mut(data.cast::<u8>(), len) }
        };
        context.0.read_tensor(unsafe { &(*tensor).0 }, data)?;
        Ok(())
    })
}

/// The builder borrows `context` until it is destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn webnn_graph_builder_create(
    context: *mut WebnnContext,
    builder: *mut *mut WebnnGraphBuilder,
) -> WebnnStatus {
    ffi_call(|| {
        let context = unsafe { &mut *context };
        let created = MLGraphBuilder::new(&mut context.0)?;
        // The context is heap allocated and outlives the builder.
        let created = unsafe {
            std::mem::transmute::<MLGraphBuilder<'_, '_>, MLGraphBuilder<'static, 'static>>(
                created,
            )
        };
        unsafe { *builder = Box::into_raw(Box::new(WebnnGraphBuilder(created))) };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn webnn_graph_builder_destroy(builder: *mut WebnnGraphBuilder) {
    unsafe { destroy(builder) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn webnn_graph_builder_input(
    builder: *mut WebnnGraphBuilder,
    name: *const c_char,
    desc: *const WebnnOperandDescriptor,
    operand: *mut u32,
) -> WebnnStatus {
    ffi_call(|| {
        let builder = unsafe { &mut *builder };
        let name = unsafe { input_str(name)? };
        let desc = unsafe { descriptor(&*desc)? };
        let created = builder.0.input(name, &desc)?;
        unsafe { *operand = created.rustnn_index() };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn webnn_graph_builder_constant(
    builder: *mut WebnnGraphBuilder,
    desc: *const WebnnOperandDescriptor,
    data: *const c_void,
    len: usize,
    operand: *mut u32,
) -> WebnnStatus {
    ffi_call(|| {
        let builder = unsafe { &mut *builder };
        let desc = unsafe { descriptor(&*desc)? };
        let data = unsafe { input_slice(data.cast::<u8>(), len)? };
        let created = builder.0.constant_from_bytes(&desc, data.to_vec())?;
        unsafe { *operand = created.rustnn_index() };
        Ok(())
    })
}

/// Compiles the graph. The builder cannot be used afterwards, and the graph
/// borrows the builder's context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn webnn_graph_builder_build(
    builder: *mut WebnnGraphBuilder,
    outputs: *const WebnnNamedOperand,
    outputs_len: usize,
    graph: *mut *mut WebnnGraph,
) -> WebnnStatus {
    ffi_call(|| {
        let builder = unsafe { &mut *builder };
        let mut named: MLNamedOperands<'_> = BTreeMap::new();
        for output in unsafe { input_slice(outputs, outputs_len)? } {
            let name = unsafe { input_str(output.name)? };
            if named.insert(name, MLOperand::from(output.operand)).is_some() {
                return Err(Error::InvalidArgument(format!("duplicate output name: {name}")));
            }
        }
        let built = builder.0.build(&named)?;
        unsafe { *graph = Box::into_raw(Box::new(WebnnGraph(built))) };
        Ok(())
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn webnn_graph_destroy(graph: *mut WebnnGraph) {
    unsafe { destroy(graph) }
}

unsafe fn named_tensors<'a>(
    tensors: *const WebnnNamedTensor,
    len: usize,
) -> Result<MLNamedTensors<'a>, Error> {
    let mut named: MLNamedTensors<'a> = BTreeMap::new();
    for entry in unsafe { input_slice(tensors, len)? } {
        let name = unsafe { input_str(entry.name)? };
        if entry.tensor.is_null() {
            return Err(Error::InvalidArgument("null tensor".to_string()));
        }
        if named.insert(name, unsafe { &(*entry.tensor).0 }).is_some() {
            return Err(Error::InvalidArgument(format!("duplicate tensor name: {name}")));
        }
    }
    Ok(named)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn webnn_context_dispatch(
    context: *mut WebnnContext,
    graph: *mut WebnnGraph,
    inputs: *const WebnnNamedTensor,
    inputs_len: usize,
    outputs: *const WebnnNamedTensor,
    outputs_len: usize,
) -> WebnnStatus {
    ffi_call(|| {
        let context = unsafe { &mut *context };
        let graph = unsafe { &mut *graph };
        let inputs = unsafe { named_tensors(inputs, inputs_len)? };
        let outputs = unsafe { named_tensors(outputs, outputs_len)? };
        context.0.dispatch(&mut graph.0, &inputs, &outputs)?;
        Ok(())
    })
}

pub(crate) fn write_outputs(
    operands: Vec<MLOperand>,
    outputs: *mut u32,
    capacity: usize,
    len: *mut usize,
) -> Result<(), Error> {
    if operands.len() > capacity {
        return Err(Error::InvalidArgument(format!(
            "{} outputs, but room for {capacity}",
            operands.len()
        )));
    }
    for (i, operand) in operands.iter().enumerate() {
        unsafe { *outputs.add(i) = operand.rustnn_index() };
    }
    unsafe { *len = operands.len() };
    Ok(())
}
