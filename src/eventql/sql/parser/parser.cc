/**
 * Copyright (c) 2016 DeepCortex GmbH <legal@eventql.io>
 * Authors:
 *   - Paul Asmuth <paul@eventql.io>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License ("the license") as
 * published by the Free Software Foundation, either version 3 of the License,
 * or any later version.
 *
 * In accordance with Section 7(e) of the license, the licensing of the Program
 * under the license does not imply a trademark license. Therefore any rights,
 * title and interest in our trademarks remain entirely with us.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the license for more details.
 *
 * You can be released from the requirements of the license by purchasing a
 * commercial license. Buying such a license is mandatory as soon as you develop
 * commercial activities involving this program without disclosing the source
 * code of your own applications
 */
#include <stdlib.h>
#include <assert.h>
#include <memory>
#include "parser.h"
#include "tokenize.h"
#include "eventql/util/inspect.h"
#include "eventql/util/json/json.h"

namespace csql {

Parser::Parser() : root_(ASTNode::T_ROOT) {}

std::vector<std::unique_ptr<ASTNode>> Parser::parseQuery(
    const std::string query) {
  if (query.size() == 0) {
    RAISE(kParseError, "empty query");
  }

  if (!parse(query.c_str(), query.size())) {
    RAISE(
        kParseError,
        "can't figure out how to parse this, sorry :(");
  }

  std::vector<std::unique_ptr<ASTNode>> stmts;
  for (const auto stmt : getStatements()) {
    stmts.emplace_back(stmt->deepCopy());
  }

  return stmts;
}

// FIXPAUL free all explicit news on ex!
size_t Parser::parse(const char* query, size_t len) {
  const char* cur = query;
  const char* end = cur + len;

  tokenizeQuery(&cur, end, &token_list_);

  if (token_list_.size() == 0) {
    RAISE(kRuntimeError, "SQL query doesn't contain any tokens");
  }

  token_list_.emplace_back(Token::T_EOF);
  cur_token_ = token_list_.data();
  token_list_end_ = cur_token_ + token_list_.size();

  while (cur_token_ < token_list_end_ &&
      cur_token_->getType() != Token::T_EOF) {
    root_.appendChild(statement());
  }

  return true;
}

size_t Parser::parseValueExpression(const char* query, size_t len) {
  const char* cur = query;
  const char* end = cur + len;

  tokenizeQuery(&cur, end, &token_list_);

  if (token_list_.size() == 0) {
    RAISE(kRuntimeError, "SQL value expression doesn't contain any tokens");
  }

  token_list_.emplace_back(Token::T_EOF);
  cur_token_ = token_list_.data();
  token_list_end_ = cur_token_ + token_list_.size();
  root_.appendChild(expectAndConsumeValueExpr());

  return true;
}

ASTNode* Parser::expr(int precedence /* = 0 */) {
  auto lhs = unaryExpr();

  if (lhs == nullptr) {
    return nullptr;
  }

  for (;;) {
    auto expr = binaryExpr(lhs, precedence);
    if (expr == nullptr) {
      return lhs;
    } else {
      lhs = expr;
    }
  }
}

ASTNode* Parser::unaryExpr() {
  switch (cur_token_->getType()) {

    /* parenthesized value expression */
    case Token::T_LPAREN: {
      consumeToken();
      auto e = expr();
      if (assertExpectation(Token::T_RPAREN)) {
        consumeToken();
      }
      return e;
    }

    /* prefix ~ ? */

    /* negated value expression */
    case Token::T_BANG:
    case Token::T_MINUS:
    case Token::T_NOT: {
      consumeToken();
      auto e = new ASTNode(ASTNode::T_NEGATE_EXPR);
      e->appendChild(expr());
      return e;
    }

    /* literal expression */
    case Token::T_TRUE:
    case Token::T_FALSE:
    case Token::T_NUMERIC:
    case Token::T_STRING:
    case Token::T_NULL: {
      auto e = new ASTNode(ASTNode::T_LITERAL);
      e->setToken(cur_token_);
      consumeToken();
      return e;
    }

    case Token::T_IDENTIFIER: {
      return columnName();
    }

    default:
      return nullptr;

  }
}

ASTNode* Parser::columnName() {
  assertExpectation(Token::T_IDENTIFIER);

  ASTNode* expr = nullptr;

  if (lookahead(1, Token::T_DOT)) {
    /* table_name.column_name */
    auto col_name = new ASTNode(ASTNode::T_COLUMN_NAME);
    auto cur = col_name;
    cur->setToken(cur_token_);
    consumeToken();
    do {
      consumeToken();
      assertExpectation(Token::T_IDENTIFIER);
      auto next = cur->appendChild(ASTNode::T_COLUMN_NAME);
      next->setToken(cur_token_);
      cur = next;
      consumeToken();
    } while (lookahead(0, Token::T_DOT));

    return col_name;
  }

  if (lookahead(1, Token::T_LPAREN)) {
    return methodCall();
  }

  /* simple column name */
  expr = new ASTNode(ASTNode::T_COLUMN_NAME);
  expr->setToken(cur_token_);
  consumeToken();
  return expr;
}

ASTNode* Parser::methodCall() {
  auto e = new ASTNode(ASTNode::T_METHOD_CALL);
  e->setToken(consumeToken());

  if (e->getToken()->getString() == "if") {
    e->setType(ASTNode::T_IF_EXPR);
  }

  if (e->getToken()->getString() == "subquery_column") {
    e->setType(ASTNode::T_COLUMN_INDEX);
  }

  /* read arguments */
  do {
    consumeToken();

    /* ignore T_ALL in SELECT count(*) FROM ... */
    if (*cur_token_ == Token::T_ASTERISK) {
      e->appendChild(ASTNode::T_VOID);
      consumeToken();
    } else if (*cur_token_ == Token::T_RPAREN) {
      break;
    } else {
      e->appendChild(expr());
    }
  } while (*cur_token_ == Token::T_COMMA);

  expectAndConsume(Token::T_RPAREN);

  if (lookahead(0, Token::T_WITHIN) &&
      lookahead(1, Token::T_RECORD)) {
    consumeToken();
    consumeToken();
    e->setType(ASTNode::T_METHOD_CALL_WITHIN_RECORD);
  }

  return e;
}

ASTNode* Parser::binaryExpr(ASTNode* lhs, int precedence) {
  switch (cur_token_->getType()) {

    /* equals expression */
    case Token::T_EQUAL:
      return eqExpr(lhs, precedence);

    /* not equals expression */
    case Token::T_NEQUAL:
      return neqExpr(lhs, precedence);

    /* less than expression */
    case Token::T_LT:
      return ltExpr(lhs, precedence);

    /* less than or equal expression */
    case Token::T_LTE:
      return lteExpr(lhs, precedence);

    /* greater than expression */
    case Token::T_GT:
      return gtExpr(lhs, precedence);

    /* greater than or equal expression */
    case Token::T_GTE:
      return gteExpr(lhs, precedence);

    /* and expression */
    case Token::T_AND:
      return andExpr(lhs, precedence);

    /* or expression */
    case Token::T_OR:
      return orExpr(lhs, precedence);

    /* add expression */
    case Token::T_PLUS:
      return addExpr(lhs, precedence);

    /* subtract expression */
    case Token::T_MINUS:
      return subExpr(lhs, precedence);

    /* multiply expression */
    case Token::T_ASTERISK:
      return mulExpr(lhs, precedence);

    /* division expression */
    case Token::T_SLASH:
    case Token::T_DIV:
      return divExpr(lhs, precedence);

    /* modulo expression */
    case Token::T_PERCENT:
    case Token::T_MOD:
      return modExpr(lhs, precedence);

    /* exponentiation expression */
    case Token::T_CIRCUMFLEX:
      return powExpr(lhs, precedence);

    /* REGEX operator */
    case Token::T_REGEX:
      return regexExpr(lhs, precedence);

    /* LIKE operator */
    case Token::T_LIKE:
      return likeExpr(lhs, precedence);

    // FIXPAUL: lshift, rshift, ampersand, pipe, tilde, noq, and, or
    default:
      return nullptr;

  }
}

ASTNode* Parser::statement() {
  switch (cur_token_->getType()) {
    case Token::T_SELECT:
      return selectStatement();
    case Token::T_CREATE:
      return createStatement();
    case Token::T_DROP:
      return dropStatement();
    case Token::T_INSERT:
      return insertStatement();
    case Token::T_ALTER:
      return alterStatement();
    case Token::T_DRAW:
      return drawStatement();
    case Token::T_IMPORT:
      return importStatement();
    case Token::T_SHOW:
      return showStatement();
    case Token::T_DESCRIBE:
    case Token::T_EXPLAIN:
      return explainStatement();
    default:
      break;
  }

  RAISE(
      kParseError,
      "unexpected token %s%s%s, expected one of SELECT, CREATE, INSERT, ALTER, DROP, DRAW or IMPORT",
        Token::getTypeName(cur_token_->getType()),
        cur_token_->getString().size() > 0 ? ": " : "",
        cur_token_->getString().c_str());

  return nullptr;
}

ASTNode* Parser::selectStatement() {
  auto select = new ASTNode(ASTNode::T_SELECT);
  consumeToken();

  /* DISTINCT/ALL */
  // FIXPAUL read SET_QUANTIFIER (distinct, all...)

  /* select list */
  auto select_list = select->appendChild(ASTNode::T_SELECT_LIST);
  if (*cur_token_ == Token::T_ASTERISK) {
    select_list->appendChild(ASTNode::T_ALL);
    consumeToken();
  } else {
    for (;;) {
      select_list->appendChild(selectSublist());

      if (*cur_token_ == Token::T_COMMA) {
        consumeToken();
      } else {
        break;
      }
    }
  }

  /* FROM clause */
  switch (cur_token_->getType()) {
    case Token::T_SEMICOLON:
    case Token::T_RPAREN:
      break;
    default:
      select->appendChild(fromClause());
      break;
  };

  /* WHERE clause */
  auto where = whereClause();
  if (where != nullptr) {
    select->appendChild(where);
  }

  /* GROUP BY clause */
  auto group = groupByClause();
  if (group != nullptr) {
    select->appendChild(group);
  }

  /* HAVING clause */
  auto having = havingClause();
  if (having != nullptr) {
    select->appendChild(having);
  }

  /* ORDER BY clause */
  auto order = orderByClause();
  if (order != nullptr) {
    select->appendChild(order);
  }

  /* LIMIT clause */
  auto limit = limitClause();
  if (limit != nullptr) {
    select->appendChild(limit);
  }

  if (*cur_token_ == Token::T_SEMICOLON) {
    consumeToken();
  }

  return select;
}

ASTNode* Parser::createStatement() {
  consumeToken();

  switch (cur_token_->getType()) {
    case Token::T_TABLE:
      return createTableStatement();
    case Token::T_DATABASE:
      return createDatabaseStatement();
    default:
      RAISEF(
        kParseError,
        "unexpected token $0$1$2, expected one of SELECT, DRAW or IMPORT",
          Token::getTypeName(cur_token_->getType()),
          cur_token_->getString().size() > 0 ? ": " : "",
          cur_token_->getString().c_str());

  }
}

ASTNode* Parser::createTableStatement() {
  expectAndConsume(Token::T_TABLE);

  auto create_table = new ASTNode(ASTNode::T_CREATE_TABLE);
  create_table->appendChild(tableName());

  auto column_list = new ASTNode(ASTNode::T_COLUMN_LIST);
  create_table->appendChild(column_list);

  expectAndConsume(Token::T_LPAREN);

  while (*cur_token_ != Token::T_RPAREN) {
    if (*cur_token_ == Token::T_PRIMARY) {
      column_list->appendChild(primaryKeyDefinition());
    } else {
      auto coldef = columnDefinition();

      if (*cur_token_ == Token::T_PRIMARY) {
        consumeToken();
        expectAndConsume(Token::T_KEY);
        coldef->appendChild(new ASTNode(ASTNode::T_PRIMARY_KEY));
      }

      column_list->appendChild(coldef);
    }

    if (*cur_token_ == Token::T_COMMA) {
      consumeToken();
    } else {
      break;
    }
  }

  expectAndConsume(Token::T_RPAREN);

  if (*cur_token_ == Token::T_WITH) {
    consumeToken();
    auto property_list = new ASTNode(ASTNode::T_TABLE_PROPERTY_LIST);
    create_table->appendChild(property_list);

    while (*cur_token_ != Token::T_SEMICOLON) {
      property_list->appendChild(tablePropertyDefinition());

      if (*cur_token_ == Token::T_AND) {
        consumeToken();
      } else {
        break;
      }
    }
  }

  if (*cur_token_ == Token::T_SEMICOLON) {
    consumeToken();
  }

  return create_table;
}

ASTNode* Parser::columnDefinition() {
  auto column = new ASTNode(ASTNode::T_COLUMN);

  assertExpectation(Token::T_IDENTIFIER);
  auto column_name = new ASTNode(ASTNode::T_COLUMN_NAME);
  column_name->setToken(cur_token_);
  column->appendChild(column_name);
  consumeToken();

  bool repeated = false;
  if (*cur_token_ == Token::T_REPEATED) {
    repeated = true;
    consumeToken();
  }

  if (*cur_token_ == Token::T_RECORD) {
    consumeToken();
    auto record_def = new ASTNode(ASTNode::T_RECORD);
    column->appendChild(record_def);

    expectAndConsume(Token::T_LPAREN);
    while (*cur_token_ != Token::T_RPAREN) {
      record_def->appendChild(columnDefinition());

      if (*cur_token_ == Token::T_COMMA) {
        consumeToken();
      } else {
        break;
      }
    }
    expectAndConsume(Token::T_RPAREN);
  } else {
    auto id = new ASTNode(ASTNode::T_COLUMN_TYPE);
    id->setToken(cur_token_);
    column->appendChild(id);
    consumeToken();

    if (*cur_token_ == Token::T_NOT) {
      consumeToken();
      expectAndConsume(Token::T_NULL);
      column->appendChild(ASTNode::T_NOT_NULL);
    }
  }

  if (repeated) {
    column->appendChild(new ASTNode(ASTNode::T_REPEATED));
  }

  return column;
}


ASTNode* Parser::primaryKeyDefinition() {
  consumeToken();
  expectAndConsume(Token::T_KEY);

  auto primary_key = new ASTNode(ASTNode::T_PRIMARY_KEY);

  expectAndConsume(Token::T_LPAREN);

  while (*cur_token_ != Token::T_RPAREN) {
    auto expr = new ASTNode(ASTNode::T_COLUMN_NAME);
    expr->setToken(cur_token_);
    consumeToken();
    primary_key->appendChild(expr);

    if (*cur_token_ == Token::T_COMMA) {
      consumeToken();
    } else {
      break;
    }
  }

  expectAndConsume(Token::T_RPAREN);

  return primary_key;
}

ASTNode* Parser::tablePropertyDefinition() {
  auto property = new ASTNode(ASTNode::T_TABLE_PROPERTY);

  switch (cur_token_->getType()) {
    case Token::T_IDENTIFIER:
    case Token::T_STRING:
      break;

    default:
      assertExpectation(Token::T_IDENTIFIER);
  }

  auto name_str = consumeToken()->getString();
  while (lookahead(0, Token::T_DOT)) {
    consumeToken();
    assertExpectation(Token::T_IDENTIFIER);
    name_str += "." + cur_token_->getString();
    consumeToken();
  }

  auto property_key = new ASTNode(ASTNode::T_TABLE_PROPERTY_KEY);
  property_key->setToken(new Token(Token::T_IDENTIFIER, name_str));
  property->appendChild(property_key);

  expectAndConsume(Token::T_EQUAL);

  auto property_value = new ASTNode(ASTNode::T_TABLE_PROPERTY_VALUE);
  switch (cur_token_->getType()) {
    case Token::T_STRING:
    case Token::T_NUMERIC:
      break;

    default:
      assertExpectation(Token::T_STRING);
  }

  property_value->setToken(cur_token_);
  property->appendChild(property_value);
  consumeToken();

  return property;
}


ASTNode* Parser::createDatabaseStatement() {
  expectAndConsume(Token::T_DATABASE);

  auto create_database = new ASTNode(ASTNode::T_CREATE_DATABASE);
  auto name = new ASTNode(ASTNode::T_DATABASE_NAME);
  name->setToken(cur_token_);
  create_database->appendChild(name);
  consumeToken();

  if (*cur_token_ == Token::T_SEMICOLON) {
    consumeToken();
  }

  return create_database;
}

ASTNode* Parser::dropStatement() {
  return dropTableStatement();
}

ASTNode* Parser::dropTableStatement() {
  consumeToken();
  expectAndConsume(Token::T_TABLE);

  auto drop_table = new ASTNode(ASTNode::T_DROP_TABLE);
  drop_table->appendChild(tableName());

  if (*cur_token_ == Token::T_SEMICOLON) {
    consumeToken();
  }

  return drop_table;
}

ASTNode* Parser::insertStatement() {
  consumeToken();
  return insertIntoStatement();
}

ASTNode* Parser::insertIntoStatement() {
  if (*cur_token_ == Token::T_INTO) {
    consumeToken();
  }

  auto insert_into = new ASTNode(ASTNode::T_INSERT_INTO);
  insert_into->appendChild(tableName());

  switch (cur_token_->getType()) {
    case Token::T_FROM:
      insert_into->appendChild(insertFromJSON());
      break;

    case Token::T_LPAREN:
      insert_into->appendChild(insertColumnList());
      insert_into->appendChild(insertValueList());
      break;

    case Token::T_VALUES:
      //empty column list
      insert_into->appendChild(new ASTNode(ASTNode::T_COLUMN_LIST));
      insert_into->appendChild(insertValueList());
      break;

    default:
      RAISEF(
          kParseError,
          "unexpected Token $0, can't build expression",
          cur_token_->getString());
  }

  consumeIf(Token::T_SEMICOLON);

  return insert_into;
}

ASTNode* Parser::insertColumnList() {
  expectAndConsume(Token::T_LPAREN);

  auto column_list = new ASTNode(ASTNode::T_COLUMN_LIST);

  while (*cur_token_ != Token::T_RPAREN) {
    assertExpectation(Token::T_IDENTIFIER);
    auto column_name = new ASTNode(ASTNode::T_COLUMN_NAME);
    column_name->setToken(cur_token_);
    consumeToken();

    column_list->appendChild(column_name);

    if (*cur_token_ == Token::T_COMMA) {
      consumeToken();
    } else {
      break;
    }
  }

  expectAndConsume(Token::T_RPAREN);

  return column_list;
}

ASTNode* Parser::insertValueList() {
  expectAndConsume(Token::T_VALUES);
  expectAndConsume(Token::T_LPAREN);

  auto value_list = new ASTNode(ASTNode::T_VALUE_LIST);

  while (*cur_token_ != Token::T_RPAREN) {
    auto value = expr();
    if (value == nullptr) {
      RAISEF(
          kParseError,
          "unexpected Token $0, can't build expression",
          cur_token_->getString());
    }

    value_list->appendChild(value);

    if (*cur_token_ == Token::T_COMMA) {
      consumeToken();
    } else {
      break;
    }
  }

  expectAndConsume(Token::T_RPAREN);

  return value_list;
}

ASTNode* Parser::insertFromJSON() {
  consumeToken();
  expectAndConsume(Token::T_JSON);
  assertExpectation(Token::T_STRING);
  auto json = new ASTNode(ASTNode::T_JSON_STRING);
  json->setToken(cur_token_);

  consumeToken();
  return json;
}

ASTNode* Parser::nestedColumnName() {
  /* column_name[.column_name...] */
  assertExpectation(Token::T_IDENTIFIER);
  auto name_str = consumeToken()->getString();
  while (lookahead(0, Token::T_DOT)) {
    consumeToken();
    assertExpectation(Token::T_IDENTIFIER);
    name_str += "." + cur_token_->getString();
    consumeToken();
  }

  auto column_name = new ASTNode(ASTNode::T_COLUMN_NAME);
  column_name->setToken(new Token(Token::T_IDENTIFIER, name_str));

  return column_name;
}

ASTNode* Parser::addColumnDefinition() {
  auto column = new ASTNode(ASTNode::T_COLUMN);
  column->appendChild(nestedColumnName());

  bool repeated = false;
  if (*cur_token_ == Token::T_REPEATED) {
    repeated = true;
    consumeToken();
  }

  if (*cur_token_ == Token::T_RECORD) {
    column->appendChild(new ASTNode(ASTNode::T_RECORD));
    consumeToken();
  } else {
    auto id = new ASTNode(ASTNode::T_COLUMN_TYPE);
    id->setToken(cur_token_);
    column->appendChild(id);
    consumeToken();
  }

  if (*cur_token_ == Token::T_NOT) {
    consumeToken();
    expectAndConsume(Token::T_NULL);
    column->appendChild(ASTNode::T_NOT_NULL);
  }

  if (repeated) {
    column->appendChild(new ASTNode(ASTNode::T_REPEATED));
  }

  return column;
}

ASTNode* Parser::alterStatement() {
  consumeToken();
  expectAndConsume(Token::T_TABLE);

  auto alter_table = new ASTNode(ASTNode::T_ALTER_TABLE);
  alter_table->appendChild(tableName());

  while (*cur_token_ != Token::T_SEMICOLON) {
    switch (cur_token_->getType()) {
      case Token::T_ADD:
        consumeToken();
        consumeIf(Token::T_COLUMN);
        alter_table->appendChild(addColumnDefinition());
        break;

      case Token::T_DROP: {
        consumeToken();
        consumeIf(Token::T_COLUMN);
        alter_table->appendChild(nestedColumnName());
        break;
      }

      default:
        RAISEF(
          kParseError,
          "unexpected token $0$1$2, expected one of ADD or DROP",
          Token::getTypeName(cur_token_->getType()),
          cur_token_->getString().size() > 0 ? ": " : "",
          cur_token_->getString().c_str());
    }

    if (*cur_token_ == Token::T_COMMA) {
      consumeToken();
    } else {
      break;
    }
  }

  consumeIf(Token::T_SEMICOLON);

  return alter_table;
}

ASTNode* Parser::importStatement() {
  auto import = new ASTNode(ASTNode::T_IMPORT);
  consumeToken();

  expectAndConsume(Token::T_TABLE);
  import->appendChild(tableName());

  if (*cur_token_ == Token::T_COMMA) {
    consumeToken();
    import->appendChild(tableName());
  }

  expectAndConsume(Token::T_FROM);
  import->appendChild(expectAndConsumeValueExpr());

  consumeIf(Token::T_SEMICOLON);
  return import;
}

ASTNode* Parser::showStatement() {
  consumeToken();
  expectAndConsume(Token::T_TABLES);

  auto stmt = new ASTNode(ASTNode::T_SHOW_TABLES);

  consumeIf(Token::T_SEMICOLON);
  return stmt;
}

ASTNode* Parser::explainStatement() {
  consumeToken();

  switch (cur_token_->getType()) {
    case Token::T_SELECT:
      return explainQueryStatement();
    default:
      return describeTableStatement();
  }
}

ASTNode* Parser::explainQueryStatement() {
  auto stmt = new ASTNode(ASTNode::T_EXPLAIN_QUERY);
  stmt->appendChild(selectStatement());
  consumeIf(Token::T_SEMICOLON);
  return stmt;
}

ASTNode* Parser::describeTableStatement() {
  auto stmt = new ASTNode(ASTNode::T_DESCRIBE_TABLE);
  stmt->appendChild(tableName());
  consumeIf(Token::T_SEMICOLON);
  return stmt;
}

// FIXPAUL move this into sql extensions
ASTNode* Parser::drawStatement() {
  auto chart = new ASTNode(ASTNode::T_DRAW);
  consumeToken();

  chart->setToken(expectAndConsume(std::vector<Token::kTokenType>{
      Token::T_AREACHART,
      Token::T_BARCHART,
      Token::T_HEATMAP,
      Token::T_HISTOGRAM,
      Token::T_POINTCHART,
      Token::T_LINECHART}));

  consumeIf(Token::T_WITH);

  while (cur_token_->getType() != Token::T_SEMICOLON) {
    switch (cur_token_->getType()) {
      case Token::T_AXIS:
        chart->appendChild(axisClause());
        break;

      case Token::T_XDOMAIN:
      case Token::T_YDOMAIN:
      case Token::T_ZDOMAIN:
        chart->appendChild(domainClause());
        break;

      case Token::T_LEGEND:
        chart->appendChild(legendClause());
        break;

      case Token::T_GRID: {
        auto grid = chart->appendChild(ASTNode::T_GRID);
        consumeToken();

        for (int i = 0; i < 2; i++) {
          switch (cur_token_->getType()) {
            case Token::T_HORIZONTAL:
            case Token::T_VERTICAL: {
              auto prop = grid->appendChild(ASTNode::T_PROPERTY);
              prop->setToken(consumeToken());
              break;
            }
            default:
              break;
          }
        }

        break;
      }

      case Token::T_ORIENTATION: {
        auto prop = chart->appendChild(ASTNode::T_PROPERTY);
        prop->setToken(consumeToken());
        prop->appendChild(ASTNode::T_PROPERTY_VALUE)->setToken(
            expectAndConsume(std::vector<Token::kTokenType>{
                Token::T_HORIZONTAL,
                Token::T_VERTICAL}));
        break;
      }

      case Token::T_STACKED:
      case Token::T_LABELS: {
        auto prop = chart->appendChild(ASTNode::T_PROPERTY);
        prop->setToken(consumeToken());
        prop->appendChild(ASTNode::T_PROPERTY_VALUE);
        break;
      }

      case Token::T_TITLE:
      case Token::T_SUBTITLE: {
        auto prop = chart->appendChild(ASTNode::T_PROPERTY);
        prop->setToken(consumeToken());
        prop->appendChild(expectAndConsumeValueExpr());
        break;
      }

      default:
        RAISE(
            kParseError,
            "unexpected token %s%s%s",
            Token::getTypeName(cur_token_->getType()),
            cur_token_->getString().size() > 0 ? ": " : "",
            cur_token_->getString().c_str());
        return nullptr;
    }
  }

  consumeIf(Token::T_SEMICOLON);
  return chart;
}

ASTNode* Parser::axisClause() {
  auto axis = new ASTNode(ASTNode::T_AXIS);
  axis->setToken(consumeToken());

  switch (cur_token_->getType()) {
    case Token::T_TOP:
    case Token::T_RIGHT:
    case Token::T_BOTTOM:
    case Token::T_LEFT:
      axis->appendChild(ASTNode::T_AXIS_POSITION)->setToken(consumeToken());
      break;

    default:
      RAISE(
          kParseError,
          "unexpected token %s%s%s, expected one of TOP, RIGHT, BOTTOM, LEFT",
          Token::getTypeName(cur_token_->getType()),
          cur_token_->getString().size() > 0 ? ": " : "",
          cur_token_->getString().c_str());
      return nullptr;
  }

  while (cur_token_->getType() != Token::T_SEMICOLON) {
    switch (cur_token_->getType()) {
      case Token::T_TITLE: {
        auto title = axis->appendChild(ASTNode::T_PROPERTY);
        title->setToken(consumeToken());
        title->appendChild(expectAndConsumeValueExpr());
        continue;
      }

      case Token::T_TICKS: {
        auto title = axis->appendChild(ASTNode::T_AXIS_LABELS);
        consumeToken();

        for (int i = 0; i < 2; i++) {
          switch (cur_token_->getType()) {

            case Token::T_INSIDE:
            case Token::T_OUTSIDE:
            case Token::T_OFF: {
              auto prop = title->appendChild(ASTNode::T_PROPERTY);
              prop->setToken(consumeToken());
              break;
            }

            case Token::T_ROTATE: {
              auto prop = title->appendChild(ASTNode::T_PROPERTY);
              prop->setToken(consumeToken());
              prop->appendChild(expectAndConsumeValueExpr());
              break;
            }

            default:
              break;

          }
        }

        continue;
      }

      default:
        break;

    }

    break;
  }

  return axis;
}

ASTNode* Parser::domainClause() {
  auto domain = new ASTNode(ASTNode::T_DOMAIN);
  domain->setToken(consumeToken());

  auto min_expr = expr();

  if (min_expr != nullptr) {
    expectAndConsume(Token::T_COMMA);
    auto scale = domain->appendChild(ASTNode::T_DOMAIN_SCALE);
    scale->appendChild(min_expr);
    scale->appendChild(expectAndConsumeValueExpr());
  }

  for (int i = 0; i < 2; i++) {
    switch (cur_token_->getType()) {
      case Token::T_INVERT:
      case Token::T_LOGARITHMIC: {
        auto prop = domain->appendChild(ASTNode::T_PROPERTY);
        prop->setToken(consumeToken());
        prop->appendChild(ASTNode::T_PROPERTY_VALUE);
        continue;
      }

      default:
        break;
    }

    break;
  }

  return domain;
}

ASTNode* Parser::legendClause() {
  auto legend = new ASTNode(ASTNode::T_LEGEND);
  consumeToken();

  for (int i = 0; i < 3; i++) {
    auto prop = legend->appendChild(ASTNode::T_PROPERTY);
    prop->setToken(expectAndConsume(std::vector<Token::kTokenType>{
      Token::T_TOP,
      Token::T_RIGHT,
      Token::T_BOTTOM,
      Token::T_LEFT,
      Token::T_INSIDE,
      Token::T_OUTSIDE}));
  }

  if (*cur_token_ == Token::T_TITLE) {
    auto prop = legend->appendChild(ASTNode::T_PROPERTY);
    prop->setToken(consumeToken());
    prop->appendChild(expectAndConsumeValueExpr());
  }

  return legend;
}

ASTNode* Parser::selectSublist() {
  /* table_name.* */
  if (lookahead(0, Token::T_IDENTIFIER) &&
      lookahead(1, Token::T_DOT) &&
      lookahead(2, Token::T_ASTERISK)) {
    auto select_all = new ASTNode(ASTNode::T_ALL);
    select_all->setToken(cur_token_);
    cur_token_ += 3;
    return select_all;
  }

  /* derived_col AS col_name */
  auto derived = new ASTNode(ASTNode::T_DERIVED_COLUMN); // FIXPAUL free on ex
  derived->appendChild(expectAndConsumeValueExpr());

  if (consumeIf(Token::T_AS)) {
    assertExpectation(Token::T_IDENTIFIER);
  }

  if (*cur_token_ == Token::T_IDENTIFIER) {
    auto column_name = derived->appendChild(ASTNode::T_COLUMN_ALIAS);
    column_name->setToken(cur_token_);
    consumeToken();
  }

  return derived;
}

ASTNode* Parser::fromClause() {
  if (!assertExpectation(Token::T_FROM)) {
    return nullptr;
  }

  consumeToken();
  return tableReference();
}


ASTNode* Parser::tableReference() {
  auto base = tableFactor();
  return joinExpression(base);
}

ASTNode* Parser::joinExpression(ASTNode* base) {
  bool natural = false;
  if (*cur_token_ == Token::T_NATURAL) {
    consumeToken();
    natural = true;
  }

  switch (cur_token_->getType()) {

    // comma join (implicit inner join)
    case Token::T_COMMA: {
      auto join = new ASTNode(ASTNode::T_INNER_JOIN);
      consumeToken();
      join->appendChild(base);
      join->appendChild(tableFactor());
      return joinExpression(join);
    }

    // inner join
    case Token::T_CROSS:
    case Token::T_INNER:
      consumeToken();
      /* fallthrough */

    case Token::T_JOIN: {
      auto join = new ASTNode(
          natural ? ASTNode::T_NATURAL_INNER_JOIN : ASTNode::T_INNER_JOIN);
      consumeToken();
      join->appendChild(base);
      join->appendChild(tableFactor());

      if (!natural) {
        auto cond = joinCondition();
        if (cond) {
          join->appendChild(cond);
        }
      }

      return joinExpression(join);
    }

    // left/right join
    case Token::T_LEFT:
    case Token::T_RIGHT: {
      auto join_type =
          *cur_token_ == Token::T_LEFT ?
          (natural ? ASTNode::T_NATURAL_LEFT_JOIN : ASTNode::T_LEFT_JOIN) :
          (natural ? ASTNode::T_NATURAL_RIGHT_JOIN : ASTNode::T_RIGHT_JOIN);

      consumeToken();
      consumeIf(Token::T_OUTER);
      expectAndConsume(Token::T_JOIN);

      std::unique_ptr<ASTNode> join(new ASTNode(join_type));
      join->appendChild(base);
      join->appendChild(tableFactor());

      if (!natural) {
        auto cond = joinCondition();
        if (!cond) {
          RAISE(kParseError, "LEFT/RIGHT JOIN needs a JOIN CONDITION");
        }

        join->appendChild(cond);
      }

      return joinExpression(join.release());
    }

    default:
      return base;
  }
}

ASTNode* Parser::joinCondition() {
  switch (cur_token_->getType()) {
    case Token::T_ON: {
      consumeToken();
      auto cond = new ASTNode(ASTNode::T_JOIN_CONDITION);
      cond->appendChild(expectAndConsumeValueExpr());
      return cond;
    }

    case Token::T_USING: {
      consumeToken();
      expectAndConsume(Token::T_LPAREN);

      auto cond = new ASTNode(ASTNode::T_JOIN_COLUMNLIST);
      do {
        cond->appendChild(columnName());
      } while (consumeIf(Token::T_COMMA));

      expectAndConsume(Token::T_RPAREN);
      return cond;
    }

    default:
      return nullptr;
  }
}

ASTNode* Parser::tableFactor() {
  std::unique_ptr<ASTNode> base(new ASTNode(ASTNode::T_FROM));

  if (*cur_token_ == Token::T_LPAREN) {
    consumeToken();
    if (*cur_token_ == Token::T_SELECT) {
      // subquery
      base->appendChild(selectStatement());
      expectAndConsume(Token::T_RPAREN);
    } else {
      // (  table_reference )
      auto table_ref = tableReference();
      expectAndConsume(Token::T_RPAREN);
      return table_ref;
    }
  } else {
    // table_name
    base->appendChild(tableName());
  }

  // [ AS ]
  consumeIf(Token::T_AS);

  // alias
  if (*cur_token_ == Token::T_IDENTIFIER) {
    auto table_alias = base->appendChild(ASTNode::T_TABLE_ALIAS);
    table_alias->setToken(cur_token_);
    consumeToken();
  }

  return base.release();
}

ASTNode* Parser::whereClause() {
  if (!consumeIf(Token::T_WHERE)) {
    return nullptr;
  }

  auto clause = new ASTNode(ASTNode::T_WHERE);
  clause->appendChild(expr());
  return clause;
}

ASTNode* Parser::groupByClause() {
  if (!consumeIf(Token::T_GROUP)) {
    return nullptr;
  }

  // FIXPAUL move to sql extensions
  if (consumeIf(Token::T_OVER)) {
    return groupOverClause();
  }

  expectAndConsume(Token::T_BY);

  auto clause = new ASTNode(ASTNode::T_GROUP_BY);

  do {
    clause->appendChild(expr());
  } while (consumeIf(Token::T_COMMA));

  return clause;
}

// FIXPAUL move to sql extensions
ASTNode* Parser::groupOverClause() {
  expectAndConsume(Token::T_TIMEWINDOW);
  expectAndConsume(Token::T_LPAREN);

  auto clause = new ASTNode(ASTNode::T_GROUP_OVER_TIMEWINDOW);
  clause->appendChild(expectAndConsumeValueExpr());
  expectAndConsume(Token::T_COMMA);

  auto group_list = new ASTNode(ASTNode::T_GROUP_BY);
  clause->appendChild(group_list);

  clause->appendChild(expectAndConsumeValueExpr());
  if (consumeIf(Token::T_COMMA)) {
    clause->appendChild(expectAndConsumeValueExpr());
  }

  expectAndConsume(Token::T_RPAREN);

  if (consumeIf(Token::T_BY)) {
    do {
      group_list->appendChild(expr());
    } while (consumeIf(Token::T_COMMA));
  }

  return clause;
}

ASTNode* Parser::havingClause() {
  if (!consumeIf(Token::T_HAVING)) {
    return nullptr;
  }

  auto clause = new ASTNode(ASTNode::T_HAVING);
  clause->appendChild(expr());
  return clause;
}

ASTNode* Parser::orderByClause() {
  if (!consumeIf(Token::T_ORDER)) {
    return nullptr;
  }

  expectAndConsume(Token::T_BY);

  auto clause = new ASTNode(ASTNode::T_ORDER_BY);

  do {
    auto spec = clause->appendChild(ASTNode::T_SORT_SPEC);
    spec->appendChild(expr());
    switch (cur_token_->getType()) {
      case Token::T_ASC:
      case Token::T_DESC:
        spec->setToken(consumeToken());
        break;
      default:
        break;
    }
  } while (consumeIf(Token::T_COMMA));

  return clause;
}

ASTNode* Parser::limitClause() {
  if (!consumeIf(Token::T_LIMIT)) {
    return nullptr;
  }

  ASTNode* clause;
  if (!assertExpectation(Token::T_NUMERIC)) {
    return nullptr;
  } else {
    clause = new ASTNode(ASTNode::T_LIMIT);
    clause->setToken(consumeToken());
  }

  if (consumeIf(Token::T_OFFSET)) {
    if (assertExpectation(Token::T_NUMERIC)) {
      auto offset = clause->appendChild(ASTNode::T_OFFSET);
      offset->setToken(consumeToken());
    }
  }

  return clause;
}

ASTNode* Parser::tableName() {
  switch (cur_token_->getType()) {
    case Token::T_IDENTIFIER:
    case Token::T_STRING:
      break;

    default:
      assertExpectation(Token::T_IDENTIFIER);

  }

  auto name_str = consumeToken()->getString();
  while (lookahead(0, Token::T_DOT)) {
    consumeToken();
    assertExpectation(Token::T_IDENTIFIER);
    name_str += "." + cur_token_->getString();
    consumeToken();
  }

  auto name = new ASTNode(ASTNode::T_TABLE_NAME);
  name->setToken(new Token(Token::T_IDENTIFIER, name_str));
  return name;
}

ASTNode* Parser::eqExpr(ASTNode* lhs, int precedence) {
  if (precedence < 6) {
    consumeToken();
  } else {
    return nullptr;
  }

  auto rhs = expr(6);
  if (rhs == nullptr) {
    RAISE(
        kRuntimeError,
        "eqExpr needs second argument. Did you type '==' instead of '='?");
  }

  auto e = new ASTNode(ASTNode::T_EQ_EXPR);
  e->appendChild(lhs);
  e->appendChild(rhs);
  return e;
}

ASTNode* Parser::neqExpr(ASTNode* lhs, int precedence) {
  if (precedence < 6) {
    consumeToken();
  } else {
    return nullptr;
  }

  auto rhs = expr(6);
  if (rhs == nullptr) {
    RAISE(kRuntimeError, "neqExpr needs second argument");
  }

  auto e = new ASTNode(ASTNode::T_NEQ_EXPR);
  e->appendChild(lhs);
  e->appendChild(rhs);
  return e;
}

ASTNode* Parser::ltExpr(ASTNode* lhs, int precedence) {
  if (precedence < 6) {
    consumeToken();
  } else {
    return nullptr;
  }

  auto rhs = expr(6);
  if (rhs == nullptr) {
    RAISE(kRuntimeError, "ltExpr needs second argument");
  }

  auto e = new ASTNode(ASTNode::T_LT_EXPR);
  e->appendChild(lhs);
  e->appendChild(rhs);
  return e;
}

ASTNode* Parser::lteExpr(ASTNode* lhs, int precedence) {
  if (precedence < 6) {
    consumeToken();
  } else {
    return nullptr;
  }

  auto rhs = expr(6);
  if (rhs == nullptr) {
    RAISE(kRuntimeError, "lteExpr needs second argument");
  }

  auto e = new ASTNode(ASTNode::T_LTE_EXPR);
  e->appendChild(lhs);
  e->appendChild(rhs);
  return e;
}

ASTNode* Parser::gtExpr(ASTNode* lhs, int precedence) {
  if (precedence < 6) {
    consumeToken();
  } else {
    return nullptr;
  }

  auto rhs = expr(6);
  if (rhs == nullptr) {
    RAISE(kRuntimeError, "gtExpr needs second argument");
  }

  auto e = new ASTNode(ASTNode::T_GT_EXPR);
  e->appendChild(lhs);
  e->appendChild(rhs);
  return e;
}

ASTNode* Parser::gteExpr(ASTNode* lhs, int precedence) {
  if (precedence < 6) {
    consumeToken();
  } else {
    return nullptr;
  }

  auto rhs = expr(6);
  if (rhs == nullptr) {
    RAISE(kRuntimeError, "gteExpr needs second argument");
  }

  auto e = new ASTNode(ASTNode::T_GTE_EXPR);
  e->appendChild(lhs);
  e->appendChild(rhs);
  return e;
}

ASTNode* Parser::andExpr(ASTNode* lhs, int precedence) {
  if (precedence < 3) {
    consumeToken();
  } else {
    return nullptr;
  }

  auto rhs = expr(3);
  if (rhs == nullptr) {
    RAISE(kRuntimeError, "andExpr needs second argument");
  }

  auto e = new ASTNode(ASTNode::T_AND_EXPR);
  e->appendChild(lhs);
  e->appendChild(rhs);
  return e;
}

ASTNode* Parser::orExpr(ASTNode* lhs, int precedence) {
  if (precedence < 1) {
    consumeToken();
  } else {
    return nullptr;
  }

  auto rhs = expr(1);
  if (rhs == nullptr) {
    RAISE(kRuntimeError, "orExpr needs second argument");
  }

  auto e = new ASTNode(ASTNode::T_OR_EXPR);
  e->appendChild(lhs);
  e->appendChild(rhs);
  return e;
}

ASTNode* Parser::addExpr(ASTNode* lhs, int precedence) {
  if (precedence < 10) {
    consumeToken();
  } else {
    return nullptr;
  }

  auto rhs = expr(10);
  if (rhs == nullptr) {
    RAISE(kRuntimeError, "addExpr needs second argument");
  }

  auto e = new ASTNode(ASTNode::T_ADD_EXPR);
  e->appendChild(lhs);
  e->appendChild(rhs);
  return e;
}

ASTNode* Parser::subExpr(ASTNode* lhs, int precedence) {
  if (precedence < 10) {
    consumeToken();
  } else {
    return nullptr;
  }

  auto rhs = expr(10);
  if (rhs == nullptr) {
    RAISE(kRuntimeError, "subExpr needs second argument");
  }

  auto e = new ASTNode(ASTNode::T_SUB_EXPR);
  e->appendChild(lhs);
  e->appendChild(rhs);
  return e;
}

ASTNode* Parser::mulExpr(ASTNode* lhs, int precedence) {
  if (precedence < 11) {
    consumeToken();
  } else {
    return nullptr;
  }

  auto rhs = expr(11);
  if (rhs == nullptr) {
    RAISE(kRuntimeError, "mulExpr needs second argument");
  }

  auto e = new ASTNode(ASTNode::T_MUL_EXPR);
  e->appendChild(lhs);
  e->appendChild(rhs);
  return e;
}

ASTNode* Parser::divExpr(ASTNode* lhs, int precedence) {
  if (precedence < 11) {
    consumeToken();
  } else {
    return nullptr;
  }

  auto rhs = expr(11);
  if (rhs == nullptr) {
    RAISE(kRuntimeError, "divExpr needs second argument");
  }

  auto e = new ASTNode(ASTNode::T_DIV_EXPR);
  e->appendChild(lhs);
  e->appendChild(rhs);
  return e;
}

ASTNode* Parser::modExpr(ASTNode* lhs, int precedence) {
  if (precedence < 11) {
    consumeToken();
  } else {
    return nullptr;
  }

  auto rhs = expr(11);
  if (rhs == nullptr) {
    RAISE(kRuntimeError, "modExpr needs second argument");
  }

  auto e = new ASTNode(ASTNode::T_MOD_EXPR);
  e->appendChild(lhs);
  e->appendChild(rhs);
  return e;
}

ASTNode* Parser::powExpr(ASTNode* lhs, int precedence) {
  if (precedence < 12) {
    consumeToken();
  } else {
    return nullptr;
  }

  auto rhs = expr(11);
  if (rhs == nullptr) {
    RAISE(kRuntimeError, "powExpr needs second argument");
  }

  auto e = new ASTNode(ASTNode::T_POW_EXPR);
  e->appendChild(lhs);
  e->appendChild(rhs);
  return e;
}

ASTNode* Parser::regexExpr(ASTNode* lhs, int precedence) {
  if (precedence < 6) {
    consumeToken();
  } else {
    return nullptr;
  }

  auto rhs = expr(6);
  if (rhs == nullptr) {
    RAISE(kRuntimeError, "REGEX operator needs second argument");
  }

  auto e = new ASTNode(ASTNode::T_REGEX_EXPR);
  e->appendChild(lhs);
  e->appendChild(rhs);
  return e;
}

ASTNode* Parser::likeExpr(ASTNode* lhs, int precedence) {
  if (precedence < 6) {
    consumeToken();
  } else {
    return nullptr;
  }

  auto rhs = expr(6);
  if (rhs == nullptr) {
    RAISE(kRuntimeError, "LIKE operator needs second argument");
  }

  auto e = new ASTNode(ASTNode::T_LIKE_EXPR);
  e->appendChild(lhs);
  e->appendChild(rhs);
  return e;
}

bool Parser::assertExpectation(Token::kTokenType expectation) {
  if (!(*cur_token_ == expectation)) {
    RAISE(
        kParseError,
        "unexpected token %s%s%s, expected: '%s'",
        Token::getTypeName(cur_token_->getType()),
        cur_token_->getString().size() > 0 ? ": " : "",
        cur_token_->getString().c_str(),
        Token::getTypeName(expectation));
    return false;
  }

  return true;
}

const std::vector<ASTNode*>& Parser::getStatements() const {
  return root_.getChildren();
}

const std::vector<Token>& Parser::getTokenList() const {
  return token_list_;
}

void Parser::debugPrint() const {
  printf("[ AST ]\n");
  root_.debugPrint(2);
}


}
