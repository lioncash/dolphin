// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>
#include <cassert>
#include <memory>
#include <string>
#include <vector>

#include "Common/StringUtil.h"
#include "InputCommon/ControlReference/ExpressionParser.h"

namespace ciface::ExpressionParser
{
namespace
{
using namespace ciface::Core;

enum class TokenType
{
  Discard,
  Invalid,
  EndOfFile,
  LeftParenthesis,
  RightParenthesis,
  AND,
  OR,
  NOT,
  Add,
  Control,
};

std::string OpName(TokenType op)
{
  switch (op)
  {
  case TokenType::AND:
    return "And";
  case TokenType::OR:
    return "Or";
  case TokenType::NOT:
    return "Not";
  case TokenType::Add:
    return "Add";
  default:
    assert(false);
    return "";
  }
}

class Token
{
public:
  TokenType type;
  ControlQualifier qualifier;

  explicit Token(TokenType type_) : type(type_) {}
  explicit Token(TokenType type_, ControlQualifier qualifier_)
      : type(type_), qualifier(std::move(qualifier_))
  {
  }
  explicit operator std::string() const
  {
    switch (type)
    {
    case TokenType::Discard:
      return "Discard";
    case TokenType::EndOfFile:
      return "EOF";
    case TokenType::LeftParenthesis:
      return "(";
    case TokenType::RightParenthesis:
      return ")";
    case TokenType::AND:
      return "&";
    case TokenType::OR:
      return "|";
    case TokenType::NOT:
      return "!";
    case TokenType::Add:
      return "+";
    case TokenType::Control:
      return std::string("Device(").append(std::string(qualifier)).append(1, ')');
    case TokenType::Invalid:
      break;
    }

    return "Invalid";
  }
};

class Lexer
{
public:
  std::string expr;
  std::string::iterator it;

  explicit Lexer(std::string expr_) : expr(std::move(expr_)) { it = expr.begin(); }
  bool FetchBacktickString(std::string& value, char otherDelim = 0)
  {
    value = "";
    while (it != expr.end())
    {
      char c = *it;
      ++it;
      if (c == '`')
        return false;
      if (c > 0 && c == otherDelim)
        return true;
      value += c;
    }
    return false;
  }

  Token GetFullyQualifiedControl()
  {
    ControlQualifier qualifier;
    std::string value;

    if (FetchBacktickString(value, ':'))
    {
      // Found colon, this is the device name
      qualifier.SetDeviceQualifier(value);
      FetchBacktickString(value);
    }

    qualifier.SetControlName(std::move(value));

    return Token(TokenType::Control, std::move(qualifier));
  }

  Token GetBarewordsControl(char c)
  {
    std::string name;
    name += c;

    while (it != expr.end())
    {
      c = *it;
      if (!isalpha(c))
        break;
      name += c;
      ++it;
    }

    ControlQualifier qualifier;
    qualifier.SetControlName(std::move(name));
    return Token(TokenType::Control, std::move(qualifier));
  }

  Token NextToken()
  {
    if (it == expr.end())
      return Token(TokenType::EndOfFile);

    char c = *it++;
    switch (c)
    {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
      return Token(TokenType::Discard);
    case '(':
      return Token(TokenType::LeftParenthesis);
    case ')':
      return Token(TokenType::RightParenthesis);
    case '&':
      return Token(TokenType::AND);
    case '|':
      return Token(TokenType::OR);
    case '!':
      return Token(TokenType::NOT);
    case '+':
      return Token(TokenType::Add);
    case '`':
      return GetFullyQualifiedControl();
    default:
      if (isalpha(c))
        return GetBarewordsControl(c);
      else
        return Token(TokenType::Invalid);
    }
  }

  ParseStatus Tokenize(std::vector<Token>& tokens)
  {
    while (true)
    {
      Token tok = NextToken();

      if (tok.type == TokenType::Discard)
        continue;

      if (tok.type == TokenType::Invalid)
      {
        tokens.clear();
        return ParseStatus::SyntaxError;
      }

      tokens.push_back(tok);

      if (tok.type == TokenType::EndOfFile)
        break;
    }
    return ParseStatus::Successful;
  }
};

class ControlExpression : public Expression
{
public:
  ControlQualifier qualifier;
  Device::Control* control = nullptr;
  // Keep a shared_ptr to the device so the control pointer doesn't become invalid
  std::shared_ptr<Device> m_device;

  explicit ControlExpression(ControlQualifier qualifier_) : qualifier(std::move(qualifier_)) {}
  ControlState GetValue() const override
  {
    if (!control)
      return 0.0;

    // Note: Inputs may return negative values in situations where opposing directions are
    // activated. We clamp off the negative values here.

    // FYI: Clamping values greater than 1.0 is purposely not done to support unbounded values in
    // the future. (e.g. raw accelerometer/gyro data)

    return std::max(0.0, control->ToInput()->GetState());
  }
  void SetValue(ControlState value) override
  {
    if (control)
      control->ToOutput()->SetState(value);
  }
  int CountNumControls() const override { return control ? 1 : 0; }
  void UpdateReferences(const ControlFinder& finder) override
  {
    m_device = finder.FindDevice(qualifier);
    control = finder.FindControl(qualifier);
  }
  explicit operator std::string() const override
  {
    return std::string("`").append(std::string(qualifier)).append(1, '`');
  }
};

class BinaryExpression : public Expression
{
public:
  TokenType op;
  std::unique_ptr<Expression> lhs;
  std::unique_ptr<Expression> rhs;

  explicit BinaryExpression(TokenType op_, std::unique_ptr<Expression>&& lhs_,
                            std::unique_ptr<Expression>&& rhs_)
      : op(op_), lhs(std::move(lhs_)), rhs(std::move(rhs_))
  {
  }

  ControlState GetValue() const override
  {
    const ControlState lhs_value = lhs->GetValue();
    const ControlState rhs_value = rhs->GetValue();
    switch (op)
    {
    case TokenType::AND:
      return std::min(lhs_value, rhs_value);
    case TokenType::OR:
      return std::max(lhs_value, rhs_value);
    case TokenType::Add:
      return std::min(lhs_value + rhs_value, 1.0);
    default:
      assert(false);
      return 0;
    }
  }

  void SetValue(ControlState value) override
  {
    // Don't do anything special with the op we have.
    // Treat "A & B" the same as "A | B".
    lhs->SetValue(value);
    rhs->SetValue(value);
  }

  int CountNumControls() const override
  {
    return lhs->CountNumControls() + rhs->CountNumControls();
  }

  void UpdateReferences(const ControlFinder& finder) override
  {
    lhs->UpdateReferences(finder);
    rhs->UpdateReferences(finder);
  }

  explicit operator std::string() const override
  {
    return OpName(op)
        .append(1, '(')
        .append(std::string(*lhs))
        .append(", ")
        .append(std::string(*rhs))
        .append(1, ')');
  }
};

class UnaryExpression : public Expression
{
public:
  TokenType op;
  std::unique_ptr<Expression> inner;

  explicit UnaryExpression(TokenType op_, std::unique_ptr<Expression>&& inner_)
      : op(op_), inner(std::move(inner_))
  {
  }
  ControlState GetValue() const override
  {
    const ControlState value = inner->GetValue();
    switch (op)
    {
    case TokenType::NOT:
      return 1.0 - value;
    default:
      assert(false);
      return 0;
    }
  }

  void SetValue(ControlState value) override
  {
    switch (op)
    {
    case TokenType::NOT:
      inner->SetValue(1.0 - value);
      break;

    default:
      assert(false);
    }
  }

  int CountNumControls() const override { return inner->CountNumControls(); }
  void UpdateReferences(const ControlFinder& finder) override { inner->UpdateReferences(finder); }
  explicit operator std::string() const override
  {
    return OpName(op).append(1, '(').append(std::string(*inner)).append(1, ')');
  }
};

// This class proxies all methods to its either left-hand child if it has bound controls, or its
// right-hand child. Its intended use is for supporting old-style barewords expressions.
class CoalesceExpression : public Expression
{
public:
  explicit CoalesceExpression(std::unique_ptr<Expression>&& lhs, std::unique_ptr<Expression>&& rhs)
      : m_lhs(std::move(lhs)), m_rhs(std::move(rhs))
  {
  }

  ControlState GetValue() const override { return GetActiveChild()->GetValue(); }
  void SetValue(ControlState value) override { GetActiveChild()->SetValue(value); }

  int CountNumControls() const override { return GetActiveChild()->CountNumControls(); }
  explicit operator std::string() const override
  {
    return std::string("Coalesce(")
        .append(std::string(*m_lhs))
        .append(", ")
        .append(std::string(*m_rhs))
        .append(1, ')');
  }

  void UpdateReferences(const ControlFinder& finder) override
  {
    m_lhs->UpdateReferences(finder);
    m_rhs->UpdateReferences(finder);
  }

private:
  const std::unique_ptr<Expression>& GetActiveChild() const
  {
    return m_lhs->CountNumControls() > 0 ? m_lhs : m_rhs;
  }

  std::unique_ptr<Expression> m_lhs;
  std::unique_ptr<Expression> m_rhs;
};

struct ParseResult
{
  explicit ParseResult(ParseStatus status_, std::unique_ptr<Expression>&& expr_ = {})
      : status(status_), expr(std::move(expr_))
  {
  }

  ParseStatus status;
  std::unique_ptr<Expression> expr;
};

class Parser
{
public:
  explicit Parser(std::vector<Token> tokens_) : tokens(std::move(tokens_))
  {
    m_it = tokens.begin();
  }
  ParseResult Parse() { return Toplevel(); }

private:
  std::vector<Token> tokens;
  std::vector<Token>::iterator m_it;

  static bool IsUnaryExpression(TokenType type)
  {
    switch (type)
    {
    case TokenType::NOT:
      return true;
    default:
      return false;
    }
  }

  static bool IsBinaryToken(TokenType type)
  {
    switch (type)
    {
    case TokenType::AND:
    case TokenType::OR:
    case TokenType::Add:
      return true;
    default:
      return false;
    }
  }

  Token Chew() { return *m_it++; }
  Token Peek() const { return *m_it; }
  bool Expects(TokenType type)
  {
    Token tok = Chew();
    return tok.type == type;
  }

  ParseResult Atom()
  {
    Token tok = Chew();
    switch (tok.type)
    {
    case TokenType::Control:
      return ParseResult{ParseStatus::Successful,
                         std::make_unique<ControlExpression>(tok.qualifier)};
    case TokenType::LeftParenthesis:
      return Paren();
    default:
      return ParseResult{ParseStatus::SyntaxError};
    }
  }

  ParseResult Unary()
  {
    if (IsUnaryExpression(Peek().type))
    {
      Token tok = Chew();
      ParseResult result = Atom();
      if (result.status == ParseStatus::SyntaxError)
        return result;
      return ParseResult{ParseStatus::Successful,
                         std::make_unique<UnaryExpression>(tok.type, std::move(result.expr))};
    }

    return Atom();
  }

  ParseResult Binary()
  {
    ParseResult result = Unary();
    if (result.status == ParseStatus::SyntaxError)
      return result;

    std::unique_ptr<Expression> expr = std::move(result.expr);
    while (IsBinaryToken(Peek().type))
    {
      Token tok = Chew();
      ParseResult unary_result = Unary();
      if (unary_result.status == ParseStatus::SyntaxError)
      {
        return unary_result;
      }

      expr = std::make_unique<BinaryExpression>(tok.type, std::move(expr),
                                                std::move(unary_result.expr));
    }

    return ParseResult{ParseStatus::Successful, std::move(expr)};
  }

  ParseResult Paren()
  {
    // lparen already chewed
    ParseResult result = Toplevel();
    if (result.status != ParseStatus::Successful)
      return result;

    if (!Expects(TokenType::RightParenthesis))
    {
      return ParseResult{ParseStatus::SyntaxError};
    }

    return result;
  }

  ParseResult Toplevel() { return Binary(); }
};

ParseResult ParseComplexExpression(const std::string& str)
{
  Lexer l(str);
  std::vector<Token> tokens;
  const ParseStatus tokenize_status = l.Tokenize(tokens);
  if (tokenize_status != ParseStatus::Successful)
    return ParseResult{tokenize_status};

  return Parser(std::move(tokens)).Parse();
}

std::unique_ptr<Expression> ParseBarewordExpression(const std::string& str)
{
  ControlQualifier qualifier;
  qualifier.SetControlName(str);

  return std::make_unique<ControlExpression>(std::move(qualifier));
}
}  // Anonymous namespace

std::shared_ptr<Device> ControlFinder::FindDevice(const ControlQualifier& qualifier) const
{
  if (qualifier.HasDevice())
    return container.FindDevice(qualifier.GetDeviceQualifier());
  else
    return container.FindDevice(default_device);
}

Device::Control* ControlFinder::FindControl(const ControlQualifier& qualifier) const
{
  const std::shared_ptr<Device> device = FindDevice(qualifier);
  if (!device)
    return nullptr;

  const auto& control_name = qualifier.GetControlName();
  if (is_input)
    return device->FindInput(control_name);
  else
    return device->FindOutput(control_name);
}

std::pair<ParseStatus, std::unique_ptr<Expression>> ParseExpression(const std::string& str)
{
  if (StripSpaces(str).empty())
    return std::make_pair(ParseStatus::EmptyExpression, nullptr);

  auto bareword_expr = ParseBarewordExpression(str);
  ParseResult complex_result = ParseComplexExpression(str);

  if (complex_result.status != ParseStatus::Successful)
  {
    return std::make_pair(complex_result.status, std::move(bareword_expr));
  }

  auto combined_expr = std::make_unique<CoalesceExpression>(std::move(bareword_expr),
                                                            std::move(complex_result.expr));
  return std::make_pair(complex_result.status, std::move(combined_expr));
}
}  // namespace ciface::ExpressionParser
