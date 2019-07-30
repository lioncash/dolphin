// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <string>
#include <utility>
#include "InputCommon/ControllerInterface/Device.h"

namespace ciface::ExpressionParser
{
class ControlQualifier
{
public:
  bool HasDevice() const { return m_has_device; }
  const Core::DeviceQualifier& GetDeviceQualifier() const { return m_device_qualifier; }
  void SetDeviceQualifier(const std::string& qualifier)
  {
    m_device_qualifier.FromString(qualifier);
    m_has_device = true;
  }

  const std::string& GetControlName() const { return m_control_name; }
  void SetControlName(std::string name) { m_control_name = std::move(name); }

  explicit operator std::string() const
  {
    if (m_has_device)
      return m_device_qualifier.ToString().append(1, ':').append(m_control_name);
    else
      return m_control_name;
  }

private:
  bool m_has_device = false;
  Core::DeviceQualifier m_device_qualifier;
  std::string m_control_name;
};

class ControlFinder
{
public:
  explicit ControlFinder(const Core::DeviceContainer& container_,
                         const Core::DeviceQualifier& default_, const bool is_input_)
      : container(container_), default_device(default_), is_input(is_input_)
  {
  }
  std::shared_ptr<Core::Device> FindDevice(const ControlQualifier& qualifier) const;
  Core::Device::Control* FindControl(const ControlQualifier& qualifier) const;

private:
  const Core::DeviceContainer& container;
  const Core::DeviceQualifier& default_device;
  bool is_input;
};

class Expression
{
public:
  virtual ~Expression() = default;
  virtual ControlState GetValue() const = 0;
  virtual void SetValue(ControlState state) = 0;
  virtual int CountNumControls() const = 0;
  virtual void UpdateReferences(const ControlFinder& finder) = 0;
  virtual explicit operator std::string() const = 0;
};

enum class ParseStatus
{
  Successful,
  SyntaxError,
  EmptyExpression,
};

std::pair<ParseStatus, std::unique_ptr<Expression>> ParseExpression(const std::string& expr);
}  // namespace ciface::ExpressionParser
