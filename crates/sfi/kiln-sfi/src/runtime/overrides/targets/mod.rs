// SPDX-License-Identifier: MIT
//! The override targets of the families that resolve a target kind (`runtime.overrides.<kind>`), each with its claim
//! factory: [`component`] (the ECS family's), [`attribute`] (the attribute family's, on the component claim),
//! [`service`] (the named-services family's), [`resource`] (the loaded-resource family's) and [`template`] (the ECS
//! family's template component views).

pub mod attribute;
pub mod component;
pub mod resource;
pub mod service;
pub mod template;
