//===--- ParsePattern.cpp - Swift Language Parser for Patterns ------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Pattern Parsing and AST Building
//
//===----------------------------------------------------------------------===//

#include "swift/Parse/CodeCompletionCallbacks.h"
#include "swift/Parse/Parser.h"
#include "swift/AST/ASTWalker.h"
#include "swift/AST/ExprHandle.h"
#include "swift/Basic/StringExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/SaveAndRestore.h"
using namespace swift;

/// \brief Determine the kind of a default argument given a parsed
/// expression that has not yet been type-checked.
static DefaultArgumentKind getDefaultArgKind(ExprHandle *init) {
  if (!init || !init->getExpr())
    return DefaultArgumentKind::None;

  auto magic = dyn_cast<MagicIdentifierLiteralExpr>(init->getExpr());
  if (!magic)
    return DefaultArgumentKind::Normal;

  switch (magic->getKind()) {
  case MagicIdentifierLiteralExpr::Column:
    return DefaultArgumentKind::Column;
  case MagicIdentifierLiteralExpr::File:
    return DefaultArgumentKind::File;
  case MagicIdentifierLiteralExpr::Line:
    return DefaultArgumentKind::Line;
  case MagicIdentifierLiteralExpr::Function:
    return DefaultArgumentKind::Function;
  case MagicIdentifierLiteralExpr::DSOHandle:
    return DefaultArgumentKind::DSOHandle;
  }
}

static void recoverFromBadSelectorArgument(Parser &P) {
  while (P.Tok.isNot(tok::eof) && P.Tok.isNot(tok::r_paren) &&
         P.Tok.isNot(tok::l_brace) && P.Tok.isNot(tok::r_brace) &&
         !P.isStartOfStmt() && !P.isStartOfDecl()) {
    P.skipSingle();
  }
  P.consumeIf(tok::r_paren);
}

void Parser::DefaultArgumentInfo::setFunctionContext(DeclContext *DC) {
  assert(DC->isLocalContext());
  for (auto context : ParsedContexts) {
    context->changeFunction(DC);
  }
}

static ParserStatus parseDefaultArgument(Parser &P,
                                   Parser::DefaultArgumentInfo *defaultArgs,
                                   unsigned argIndex,
                                   ExprHandle *&init) {
  SourceLoc equalLoc = P.consumeToken(tok::equal);

  // Enter a fresh default-argument context with a meaningless parent.
  // We'll change the parent to the function later after we've created
  // that declaration.
  auto initDC =
    P.Context.createDefaultArgumentContext(P.CurDeclContext, argIndex);
  Parser::ParseFunctionBody initScope(P, initDC);

  ParserResult<Expr> initR = P.parseExpr(diag::expected_init_value);

  // Give back the default-argument context if we didn't need it.
  if (!initScope.hasClosures()) {
    P.Context.destroyDefaultArgumentContext(initDC);

  // Otherwise, record it if we're supposed to accept default
  // arguments here.
  } else if (defaultArgs) {
    defaultArgs->ParsedContexts.push_back(initDC);
  }

  if (!defaultArgs) {
    auto inFlight = P.diagnose(equalLoc, diag::non_func_decl_pattern_init);
    if (initR.isNonNull())
      inFlight.fixItRemove(SourceRange(equalLoc, initR.get()->getEndLoc()));
    return ParserStatus();
  }
  
  defaultArgs->HasDefaultArgument = true;

  if (initR.hasCodeCompletion()) {
    recoverFromBadSelectorArgument(P);
    return makeParserCodeCompletionStatus();
  }
  if (initR.isNull()) {
    recoverFromBadSelectorArgument(P);
    return makeParserError();
  }

  init = ExprHandle::get(P.Context, initR.get());
  return ParserStatus();
}

/// Determine whether we are at the start of a parameter name when
/// parsing a parameter.
static bool startsParameterName(Parser &parser, bool isClosure) {
  // '_' cannot be a type, so it must be a parameter name.
  if (parser.Tok.is(tok::kw__))
    return true;

  // To have a parameter name here, we need a name.
  if (!parser.Tok.canBeArgumentLabel())
    return false;

  // If the next token can be an argument label or is ':', this is a name.
  const auto &nextTok = parser.peekToken();
  if (nextTok.is(tok::colon) || nextTok.canBeArgumentLabel())
    return true;

  // The identifier could be a name or it could be a type. In a closure, we
  // assume it's a name, because the type can be inferred. Elsewhere, we
  // assume it's a type.
  return isClosure;
}

ParserStatus
Parser::parseParameterClause(SourceLoc &leftParenLoc,
                             SmallVectorImpl<ParsedParameter> &params,
                             SourceLoc &rightParenLoc,
                             DefaultArgumentInfo *defaultArgs,
                             ParameterContextKind paramContext) {
  assert(params.empty() && leftParenLoc.isInvalid() &&
         rightParenLoc.isInvalid() && "Must start with empty state");

  // Consume the starting '(';
  leftParenLoc = consumeToken(tok::l_paren);

  // Trivial case: empty parameter list.
  if (Tok.is(tok::r_paren)) {
    rightParenLoc = consumeToken(tok::r_paren);
    return ParserStatus();
  }

  // Parse the parameter list.
  bool isClosure = paramContext == ParameterContextKind::Closure;
  return parseList(tok::r_paren, leftParenLoc, rightParenLoc, tok::comma,
                      /*OptionalSep=*/false, /*AllowSepAfterLast=*/false,
                      diag::expected_rparen_parameter,
                      [&]() -> ParserStatus {
    ParsedParameter param;
    ParserStatus status;
    SourceLoc StartLoc = Tok.getLoc();

    unsigned defaultArgIndex = defaultArgs? defaultArgs->NextIndex++ : 0;

    // Attributes.
    bool FoundCCToken;
    parseDeclAttributeList(param.Attrs, FoundCCToken,
                          /*stop at type attributes*/true, true);
    if (FoundCCToken) {
      if (CodeCompletion) {
        CodeCompletion->completeDeclAttrKeyword(nullptr, isInSILMode(), true);
      } else {
        status |= makeParserCodeCompletionStatus();
      }
    }

    // ('inout' | 'let' | 'var')?
    if (Tok.is(tok::kw_inout)) {
      param.LetVarInOutLoc = consumeToken();
      param.SpecifierKind = ParsedParameter::InOut;
    } else if (Tok.is(tok::kw_let)) {
      diagnose(Tok.getLoc(), diag::var_not_allowed_in_pattern,
               Tok.is(tok::kw_let)).fixItRemove(Tok.getLoc());
      param.LetVarInOutLoc = consumeToken();
      param.SpecifierKind = ParsedParameter::Let;
    } else if (Tok.is(tok::kw_var)) {
      diagnose(Tok.getLoc(), diag::var_not_allowed_in_pattern,
               Tok.is(tok::kw_let)).fixItRemove(Tok.getLoc());
      param.LetVarInOutLoc = consumeToken();
      param.SpecifierKind = ParsedParameter::Var;
    }

    // Redundant specifiers are fairly common, recognize, reject, and recover
    // from this gracefully.
    if (Tok.isAny(tok::kw_inout, tok::kw_let, tok::kw_var)) {
      diagnose(Tok, diag::parameter_inout_var_let)
        .fixItRemove(Tok.getLoc());
      consumeToken();
    }

    if (startsParameterName(*this, isClosure)) {
      // identifier-or-none for the first name
      if (Tok.is(tok::kw__)) {
        param.FirstNameLoc = consumeToken();
      } else {
        assert(Tok.canBeArgumentLabel() && "startsParameterName() lied");
        param.FirstName = Context.getIdentifier(Tok.getText());
        param.FirstNameLoc = consumeToken();
      }

      // identifier-or-none? for the second name
      if (Tok.canBeArgumentLabel()) {
        if (!Tok.is(tok::kw__))
          param.SecondName = Context.getIdentifier(Tok.getText());

        param.SecondNameLoc = consumeToken();
      }

      // Operators cannot have API names.
      if (paramContext == ParameterContextKind::Operator &&
          !param.FirstName.empty() &&
          param.SecondNameLoc.isValid()) {
        diagnose(param.FirstNameLoc, 
                 diag::parameter_operator_keyword_argument)
          .fixItRemoveChars(param.FirstNameLoc, param.SecondNameLoc);
        param.FirstName = param.SecondName;
        param.FirstNameLoc = param.SecondNameLoc;
        param.SecondName = Identifier();
        param.SecondNameLoc = SourceLoc();
      }

      // (':' type)?
      if (Tok.is(tok::colon)) {
        param.ColonLoc = consumeToken();

        auto type = parseType(diag::expected_parameter_type);
        status |= type;
        param.Type = type.getPtrOrNull();
        // Only allow 'inout' before the parameter name.
        if (auto InOutTy = dyn_cast_or_null<InOutTypeRepr>(param.Type)) {
          SourceLoc InOutLoc = InOutTy->getInOutLoc();
          SourceLoc NameLoc = param.FirstNameLoc;
          diagnose(InOutLoc, diag::inout_must_appear_before_param)
              .fixItRemove(InOutLoc)
              .fixItInsert(NameLoc, "inout ");
          param.Type = InOutTy->getBase();
        }
      }
    } else {
      SourceLoc typeStartLoc = Tok.getLoc();
      auto type = parseType(diag::expected_parameter_type, false);
      status |= type;
      param.Type = type.getPtrOrNull();

      // Unnamed parameters must be written as "_: Type".
      if (param.Type) {
        diagnose(typeStartLoc, diag::parameter_unnamed)
          .fixItInsert(typeStartLoc, "_: ");
      }
    }

    // '...'?
    if (Tok.isEllipsis()) {
      param.EllipsisLoc = consumeToken();
    }

    // ('=' expr)?
    if (Tok.is(tok::equal)) {
      param.EqualLoc = Tok.getLoc();
      status |= parseDefaultArgument(*this, defaultArgs, defaultArgIndex,
                                     param.DefaultArg);

      if (param.EllipsisLoc.isValid()) {
        // The range of the complete default argument.
        SourceRange defaultArgRange;
        if (param.DefaultArg) {
          if (auto init = param.DefaultArg->getExpr()) {
            defaultArgRange = SourceRange(param.EllipsisLoc, init->getEndLoc());
          }
        }

        diagnose(param.EqualLoc, diag::parameter_vararg_default)
          .highlight(param.EllipsisLoc)
          .fixItRemove(defaultArgRange);
      }
    }

    // If we haven't made progress, don't add the param.
    if (Tok.getLoc() == StartLoc)
      return status;

    params.push_back(param);
    return status;
  });
}

/// Map parsed parameters to a ParameterList.
static ParameterList *
mapParsedParameters(Parser &parser,
                    SourceLoc leftParenLoc,
                    MutableArrayRef<Parser::ParsedParameter> params,
                    SourceLoc rightParenLoc,
                    bool isFirstParameterClause,
                    SmallVectorImpl<Identifier> *argNames,
                    Parser::ParameterContextKind paramContext) {
  auto &ctx = parser.Context;

  // Local function to create a pattern for a single parameter.
  auto createParamPattern = [&](SourceLoc &letVarInOutLoc,
                        Parser::ParsedParameter::SpecifierKindTy &specifierKind,
                                Identifier argName, SourceLoc argNameLoc,
                                Identifier paramName, SourceLoc paramNameLoc,
                                TypeRepr *type,
                                const DeclAttributes &Attrs) -> Parameter {
    Parameter param;
    bool isLet = specifierKind == Parser::ParsedParameter::Let;
    param.decl = new (ctx) ParamDecl(isLet, argNameLoc, argName,
                                     paramNameLoc, paramName, Type(),
                                     parser.CurDeclContext);
    param.decl->getAttrs() = Attrs;
    
    if (argNameLoc.isInvalid() && paramNameLoc.isInvalid())
      param.decl->setImplicit();
    
    // If a type was provided, create the typed pattern.
    if (type) {
      // If 'inout' was specified, turn the type into an in-out type.
      if (specifierKind == Parser::ParsedParameter::InOut)
        type = new (ctx) InOutTypeRepr(type, letVarInOutLoc);

      param.type = TypeLoc(type);
    } else if (specifierKind == Parser::ParsedParameter::InOut) {
      parser.diagnose(letVarInOutLoc, diag::inout_must_have_type);
      letVarInOutLoc = SourceLoc();
      specifierKind = Parser::ParsedParameter::Let;
    }
    return param;
  };

  // Collect the elements of the tuple patterns for argument and body
  // parameters.
  SmallVector<Parameter, 4> elements;
  SourceLoc ellipsisLoc;
  bool isFirstParameter = true;
  for (auto &param : params) {
    // Whether the provided name is API by default depends on the parameter
    // context.
    bool isKeywordArgumentByDefault;
    switch (paramContext) {
    case Parser::ParameterContextKind::Closure:
    case Parser::ParameterContextKind::Subscript:
    case Parser::ParameterContextKind::Operator:
      isKeywordArgumentByDefault = !isFirstParameterClause;
      break;

    case Parser::ParameterContextKind::Initializer:
      isKeywordArgumentByDefault = true;
      break;

    case Parser::ParameterContextKind::Function:
      isKeywordArgumentByDefault = !isFirstParameterClause || !isFirstParameter;
      break;

    case Parser::ParameterContextKind::Curried:
      isKeywordArgumentByDefault = true;
      break;
    }

    // Create the pattern.
    Parameter result;
    Identifier argName;
    Identifier paramName;
    if (param.SecondNameLoc.isValid()) {
      argName = param.FirstName;
      paramName = param.SecondName;

      // Both names were provided, so pass them in directly.
      result = createParamPattern(param.LetVarInOutLoc, param.SpecifierKind,
                                   argName, param.FirstNameLoc,
                                   paramName, param.SecondNameLoc,
                                   param.Type, param.Attrs);

      // If the first name is empty and this parameter would not have been
      // an API name by default, complain.
      if (param.FirstName.empty() && !isKeywordArgumentByDefault) {
        parser.diagnose(param.FirstNameLoc,
                        diag::parameter_extraneous_empty_name,
                        param.SecondName)
          .fixItRemoveChars(param.FirstNameLoc, param.SecondNameLoc);

        param.FirstNameLoc = SourceLoc();
      }

      // If the first and second names are equivalent and non-empty, and we
      // would have an argument label by default, complain.
      if (isKeywordArgumentByDefault && param.FirstName == param.SecondName
          && !param.FirstName.empty()) {
        parser.diagnose(param.FirstNameLoc,
                        diag::parameter_extraneous_double_up,
                        param.FirstName)
          .fixItRemoveChars(param.FirstNameLoc, param.SecondNameLoc);
      }
    } else {
      if (isKeywordArgumentByDefault)
        argName = param.FirstName;
      paramName = param.FirstName;

      result = createParamPattern(param.LetVarInOutLoc, param.SpecifierKind,
                                  argName, SourceLoc(),
                                  param.FirstName, param.FirstNameLoc,
                                  param.Type, param.Attrs);
    }

    // If this parameter had an ellipsis, check whether it's the last parameter.
    if (param.EllipsisLoc.isValid()) {
      if (ellipsisLoc.isValid()) {
        parser.diagnose(param.EllipsisLoc, diag::multiple_parameter_ellipsis)
          .highlight(ellipsisLoc)
          .fixItRemove(param.EllipsisLoc);

        param.EllipsisLoc = SourceLoc();
      } else if (!result.type.getTypeRepr()) {
        parser.diagnose(param.EllipsisLoc, diag::untyped_pattern_ellipsis)
          .highlight(result.getSourceRange());

        param.EllipsisLoc = SourceLoc();
      } else {
        ellipsisLoc = param.EllipsisLoc;
        result.setVariadic();
      }
    }

    if (param.DefaultArg) {
      if (!isFirstParameterClause) {
        // Default arguments are only permitted on the first parameter clause.
        parser.diagnose(param.EqualLoc, diag::non_func_decl_pattern_init)
          .fixItRemove(SourceRange(param.EqualLoc,
                                   param.DefaultArg->getExpr()->getEndLoc()));
      } else {
        result.defaultArgumentKind = getDefaultArgKind(param.DefaultArg);
        result.setDefaultValue(param.DefaultArg);
      }
    }

    elements.push_back(result);

    if (argNames)
      argNames->push_back(argName);

    isFirstParameter = false;
  }

  return ParameterList::create(ctx, leftParenLoc, elements, rightParenLoc);
}

/// Parse a single parameter-clause.
ParserResult<ParameterList> Parser::parseSingleParameterClause(
                                ParameterContextKind paramContext,
                                SmallVectorImpl<Identifier> *namePieces) {
  ParserStatus status;
  SmallVector<ParsedParameter, 4> params;
  SourceLoc leftParenLoc, rightParenLoc;
  
  // Parse the parameter clause.
  status |= parseParameterClause(leftParenLoc, params, rightParenLoc,
                                 /*defaultArgs=*/nullptr, paramContext);
  
  // Turn the parameter clause into argument and body patterns.
  auto paramList = mapParsedParameters(*this, leftParenLoc, params,
                                     rightParenLoc, true, namePieces,
                                     paramContext);

  return makeParserResult(status, paramList);
}

/// Parse function arguments.
///   func-arguments:
///     curried-arguments | selector-arguments
///   curried-arguments:
///     parameter-clause+
///   selector-arguments:
///     '(' selector-element ')' (identifier '(' selector-element ')')+
///   selector-element:
///      identifier '(' pattern-atom (':' type)? ('=' expr)? ')'
///
ParserStatus
Parser::parseFunctionArguments(SmallVectorImpl<Identifier> &NamePieces,
                               SmallVectorImpl<ParameterList*> &BodyParams,
                               ParameterContextKind paramContext,
                               DefaultArgumentInfo &DefaultArgs) {
  // Parse parameter-clauses.
  ParserStatus status;
  bool isFirstParameterClause = true;
  unsigned FirstBodyPatternIndex = BodyParams.size();
  while (Tok.is(tok::l_paren)) {
    SmallVector<ParsedParameter, 4> params;
    SourceLoc leftParenLoc, rightParenLoc;

    // Parse the parameter clause.
    status |= parseParameterClause(leftParenLoc, params, rightParenLoc,
                                   &DefaultArgs, paramContext);

    // Turn the parameter clause into argument and body patterns.
    auto pattern = mapParsedParameters(*this, leftParenLoc, params,
                                       rightParenLoc, 
                                       isFirstParameterClause,
                                       isFirstParameterClause ? &NamePieces
                                                              : nullptr,
                                       paramContext);
    BodyParams.push_back(pattern);
    isFirstParameterClause = false;
    paramContext = ParameterContextKind::Curried;
  }

  // If the decl uses currying syntax, warn that that syntax is going away.
  if (BodyParams.size() - FirstBodyPatternIndex > 1) {
    SourceRange allPatternsRange(
      BodyParams[FirstBodyPatternIndex]->getStartLoc(),
      BodyParams.back()->getEndLoc());
    auto diag = diagnose(allPatternsRange.Start,
                         diag::parameter_curry_syntax_removed);
    diag.highlight(allPatternsRange);
    bool seenArg = false;
    for (unsigned i = FirstBodyPatternIndex; i < BodyParams.size() - 1; i++) {
      // Replace ")(" with ", ", so "(x: Int)(y: Int)" becomes
      // "(x: Int, y: Int)". But just delete them if they're not actually
      // separating any arguments, e.g. in "()(y: Int)".
      StringRef replacement(", ");
      auto *leftParamList = BodyParams[i];
      auto *rightParamList = BodyParams[i + 1];
      if (leftParamList->size() != 0)
        seenArg = true;
      if (!seenArg || rightParamList->size() == 0)
        replacement = "";
    
      diag.fixItReplace(SourceRange(leftParamList->getEndLoc(),
                                    rightParamList->getStartLoc()),
                        replacement);
    }
  }

  return status;
}

/// Parse a function definition signature.
///   func-signature:
///     func-arguments func-throws? func-signature-result?
///   func-signature-result:
///     '->' type
///
/// Note that this leaves retType as null if unspecified.
ParserStatus
Parser::parseFunctionSignature(Identifier SimpleName,
                               DeclName &FullName,
                               SmallVectorImpl<ParameterList*> &bodyParams,
                               DefaultArgumentInfo &defaultArgs,
                               SourceLoc &throwsLoc,
                               bool &rethrows,
                               TypeRepr *&retType) {
  SmallVector<Identifier, 4> NamePieces;
  NamePieces.push_back(SimpleName);
  FullName = SimpleName;
  
  ParserStatus Status;
  // We force first type of a func declaration to be a tuple for consistency.
  if (Tok.is(tok::l_paren)) {
    ParameterContextKind paramContext;
    if (SimpleName.isOperator())
      paramContext = ParameterContextKind::Operator;
    else
      paramContext = ParameterContextKind::Function;

    Status = parseFunctionArguments(NamePieces, bodyParams, paramContext,
                                    defaultArgs);
    FullName = DeclName(Context, SimpleName, 
                        llvm::makeArrayRef(NamePieces.begin() + 1,
                                           NamePieces.end()));

    if (bodyParams.empty()) {
      // If we didn't get anything, add a () pattern to avoid breaking
      // invariants.
      assert(Status.hasCodeCompletion() || Status.isError());
      bodyParams.push_back(ParameterList::createEmpty(Context));
    }
  } else {
    diagnose(Tok, diag::func_decl_without_paren);
    Status = makeParserError();

    // Recover by creating a '() -> ?' signature.
    bodyParams.push_back(ParameterList::createEmpty(Context, PreviousLoc,
                                                    PreviousLoc));
    FullName = DeclName(Context, SimpleName, bodyParams.back());
  }
  
  // Check for the 'throws' keyword.
  rethrows = false;
  if (Tok.is(tok::kw_throws)) {
    throwsLoc = consumeToken();
  } else if (Tok.is(tok::kw_rethrows)) {
    throwsLoc = consumeToken();
    rethrows = true;
  } else if (Tok.is(tok::kw_throw)) {
    throwsLoc = consumeToken();
    diagnose(throwsLoc, diag::throw_in_function_type)
      .fixItReplace(throwsLoc, "throws");
  }

  SourceLoc arrowLoc;
  // If there's a trailing arrow, parse the rest as the result type.
  if (Tok.isAny(tok::arrow, tok::colon)) {
    if (!consumeIf(tok::arrow, arrowLoc)) {
      // FixIt ':' to '->'.
      diagnose(Tok, diag::func_decl_expected_arrow)
          .fixItReplace(SourceRange(Tok.getLoc()), "->");
      arrowLoc = consumeToken(tok::colon);
    }

    ParserResult<TypeRepr> ResultType =
      parseType(diag::expected_type_function_result);
    if (ResultType.hasCodeCompletion())
      return ResultType;
    retType = ResultType.getPtrOrNull();
    if (!retType) {
      Status.setIsParseError();
      return Status;
    }
  } else {
    // Otherwise, we leave retType null.
    retType = nullptr;
  }

  // Check for 'throws' and 'rethrows' after the type and correct it.
  if (!throwsLoc.isValid()) {
    if (Tok.is(tok::kw_throws)) {
      throwsLoc = consumeToken();
    } else if (Tok.is(tok::kw_rethrows)) {
      throwsLoc = consumeToken();
      rethrows = true;
    }

    if (throwsLoc.isValid()) {
      assert(arrowLoc.isValid());
      assert(retType);
      auto diag = rethrows ? diag::rethrows_after_function_result
                           : diag::throws_after_function_result;
      SourceLoc typeEndLoc = Lexer::getLocForEndOfToken(SourceMgr,
                                                        retType->getEndLoc());
      SourceLoc throwsEndLoc = Lexer::getLocForEndOfToken(SourceMgr, throwsLoc);
      diagnose(Tok, diag)
        .fixItInsert(arrowLoc, rethrows ? "rethrows " : "throws ")
        .fixItRemoveChars(typeEndLoc, throwsEndLoc);
    }
  }

  return Status;
}

ParserStatus
Parser::parseConstructorArguments(DeclName &FullName,
                                  ParameterList *&BodyParams,
                                  DefaultArgumentInfo &DefaultArgs) {
  // If we don't have the leading '(', complain.
  if (!Tok.is(tok::l_paren)) {
    // Complain that we expected '('.
    {
      auto diag = diagnose(Tok, diag::expected_lparen_initializer);
      if (Tok.is(tok::l_brace))
        diag.fixItInsert(Tok.getLoc(), "() ");
    }

    // Create an empty parameter list to recover.
    BodyParams = ParameterList::createEmpty(Context, PreviousLoc, PreviousLoc);
    FullName = DeclName(Context, Context.Id_init, BodyParams);
    return makeParserError();
  }

  // Parse the parameter-clause.
  SmallVector<ParsedParameter, 4> params;
  SourceLoc leftParenLoc, rightParenLoc;
  
  // Parse the parameter clause.
  ParserStatus status 
    = parseParameterClause(leftParenLoc, params, rightParenLoc,
                           &DefaultArgs, ParameterContextKind::Initializer);

  // Turn the parameter clause into argument and body patterns.
  llvm::SmallVector<Identifier, 2> namePieces;
  BodyParams = mapParsedParameters(*this, leftParenLoc, params,
                                   rightParenLoc,
                                   /*isFirstParameterClause=*/true,
                                   &namePieces,
                                   ParameterContextKind::Initializer);

  FullName = DeclName(Context, Context.Id_init, namePieces);
  return status;
}


/// Parse a pattern with an optional type annotation.
///
///  typed-pattern ::= pattern (':' type)?
///
ParserResult<Pattern> Parser::parseTypedPattern() {
  auto result = parsePattern();
  
  // Now parse an optional type annotation.
  if (consumeIf(tok::colon)) {
    if (result.isNull())  // Recover by creating AnyPattern.
      result = makeParserErrorResult(new (Context) AnyPattern(PreviousLoc));
    
    ParserResult<TypeRepr> Ty = parseType();
    if (Ty.hasCodeCompletion())
      return makeParserCodeCompletionResult<Pattern>();
    if (Ty.isNull())
      Ty = makeParserResult(new (Context) ErrorTypeRepr(PreviousLoc));
    
    result = makeParserResult(result,
                            new (Context) TypedPattern(result.get(), Ty.get()));
  }
  
  return result;
}

/// Parse a pattern.
///   pattern ::= identifier
///   pattern ::= '_'
///   pattern ::= pattern-tuple
///   pattern ::= 'var' pattern
///   pattern ::= 'let' pattern
///
ParserResult<Pattern> Parser::parsePattern() {
  switch (Tok.getKind()) {
  case tok::l_paren:
    return parsePatternTuple();
    
  case tok::kw__:
    return makeParserResult(new (Context) AnyPattern(consumeToken(tok::kw__)));
    
  case tok::identifier: {
    Identifier name;
    SourceLoc loc = consumeIdentifier(&name);
    bool isLet = InVarOrLetPattern != IVOLP_InVar;
    return makeParserResult(createBindingFromPattern(loc, name, isLet));
  }
    
  case tok::code_complete:
    if (!CurDeclContext->isNominalTypeOrNominalTypeExtensionContext()) {
      // This cannot be an overridden property, so just eat the token. We cannot
      // code complete anything here -- we expect an identifier.
      consumeToken(tok::code_complete);
    }
    return nullptr;

  case tok::kw_var:
  case tok::kw_let: {
    bool isLetKeyword = Tok.is(tok::kw_let);
    bool alwaysImmutable = InVarOrLetPattern == IVOLP_AlwaysImmutable;
    bool implicitlyImmutable = InVarOrLetPattern == IVOLP_ImplicitlyImmutable;
    SourceLoc varLoc = consumeToken();

    // 'var' and 'let' patterns shouldn't nest.
    if (InVarOrLetPattern == IVOLP_InLet ||
        InVarOrLetPattern == IVOLP_InVar)
      diagnose(varLoc, diag::var_pattern_in_var, unsigned(isLetKeyword));

    if (isLetKeyword) {
      // 'let' isn't valid inside an implicitly immutable or always
      // immutable context.
      if (alwaysImmutable || implicitlyImmutable)
        diagnose(varLoc, diag::let_pattern_in_immutable_context)
          .fixItRemove(varLoc);
    } else {
      // In an always immutable context, `var` is not allowed.
      if (alwaysImmutable)
        diagnose(varLoc, diag::var_not_allowed_in_pattern, isLetKeyword)
        .fixItRemove(varLoc);
    }
    
    // In our recursive parse, remember that we're in a var/let pattern.
    llvm::SaveAndRestore<decltype(InVarOrLetPattern)>
    T(InVarOrLetPattern, isLetKeyword ? IVOLP_InLet : IVOLP_InVar);
    
    ParserResult<Pattern> subPattern = parsePattern();
    if (subPattern.hasCodeCompletion())
      return makeParserCodeCompletionResult<Pattern>();
    if (subPattern.isNull())
      return nullptr;
    return makeParserResult(new (Context) VarPattern(varLoc,
      isLetKeyword || alwaysImmutable,
      subPattern.get()));
  }
      
  default:
    if (Tok.isKeyword() &&
        (peekToken().is(tok::colon) || peekToken().is(tok::equal))) {
      diagnose(Tok, diag::expected_pattern_is_keyword, Tok.getText());
      SourceLoc Loc = Tok.getLoc();
      consumeToken();
      return makeParserErrorResult(new (Context) AnyPattern(Loc));
    }
    diagnose(Tok, diag::expected_pattern);
    return nullptr;
  }
}

Pattern *Parser::createBindingFromPattern(SourceLoc loc, Identifier name,
                                          bool isLet) {
  VarDecl *var;
  if (ArgumentIsParameter) {
    var = new (Context) ParamDecl(isLet, loc, name, loc, name, Type(),
                                  CurDeclContext);
  } else {
    var = new (Context) VarDecl(/*static*/ false, /*IsLet*/ isLet,
                                loc, name, Type(), CurDeclContext);
  }
  return new (Context) NamedPattern(var);
}

/// Parse an element of a tuple pattern.
///
///   pattern-tuple-element:
///     (identifier ':')? pattern
std::pair<ParserStatus, Optional<TuplePatternElt>>
Parser::parsePatternTupleElement() {
  // If this element has a label, parse it.
  Identifier Label;
  SourceLoc LabelLoc;

  // If the tuple element has a label, parse it.
  if (Tok.is(tok::identifier) && peekToken().is(tok::colon)) {
    LabelLoc = consumeIdentifier(&Label);
    consumeToken(tok::colon);
  }

  // Parse the pattern.
  ParserResult<Pattern>  pattern = parsePattern();
  if (pattern.hasCodeCompletion())
    return std::make_pair(makeParserCodeCompletionStatus(), None);
  if (pattern.isNull())
    return std::make_pair(makeParserError(), None);

  auto Elt = TuplePatternElt(Label, LabelLoc, pattern.get());
  return std::make_pair(makeParserSuccess(), Elt);
}

/// Parse a tuple pattern.
///
///   pattern-tuple:
///     '(' pattern-tuple-body? ')'
///   pattern-tuple-body:
///     pattern-tuple-element (',' pattern-tuple-body)*
ParserResult<Pattern> Parser::parsePatternTuple() {
  StructureMarkerRAII ParsingPatternTuple(*this, Tok);
  SourceLoc LPLoc = consumeToken(tok::l_paren);
  SourceLoc RPLoc;

  // Parse all the elements.
  SmallVector<TuplePatternElt, 8> elts;
  ParserStatus ListStatus =
    parseList(tok::r_paren, LPLoc, RPLoc, tok::comma, /*OptionalSep=*/false,
              /*AllowSepAfterLast=*/false,
              diag::expected_rparen_tuple_pattern_list,
              [&] () -> ParserStatus {
    // Parse the pattern tuple element.
    ParserStatus EltStatus;
    Optional<TuplePatternElt> elt;
    std::tie(EltStatus, elt) = parsePatternTupleElement();
    if (EltStatus.hasCodeCompletion())
      return makeParserCodeCompletionStatus();
    if (!elt)
      return makeParserError();

    // Add this element to the list.
    elts.push_back(*elt);
    return makeParserSuccess();
  });

  return makeParserResult(
           ListStatus,
           TuplePattern::createSimple(Context, LPLoc, elts, RPLoc));
}

/// Parse an optional type annotation on a pattern.
///
///  pattern-type-annotation ::= (':' type)?
///
ParserResult<Pattern> Parser::
parseOptionalPatternTypeAnnotation(ParserResult<Pattern> result,
                                   bool isOptional) {

  // Parse an optional type annotation.
  if (!consumeIf(tok::colon))
    return result;

  Pattern *P;
  if (result.isNull())  // Recover by creating AnyPattern.
    P = new (Context) AnyPattern(Tok.getLoc());
  else
    P = result.get();

  ParserResult<TypeRepr> Ty = parseType();
  if (Ty.hasCodeCompletion())
    return makeParserCodeCompletionResult<Pattern>();

  TypeRepr *repr = Ty.getPtrOrNull();
  if (!repr)
    repr = new (Context) ErrorTypeRepr(PreviousLoc);

  // In an if-let, the actual type of the expression is Optional of whatever
  // was written.
  if (isOptional)
    repr = new (Context) OptionalTypeRepr(repr, Tok.getLoc());

  return makeParserResult(new (Context) TypedPattern(P, repr));
}



/// matching-pattern ::= 'is' type
/// matching-pattern ::= matching-pattern-var
/// matching-pattern ::= expr
///
ParserResult<Pattern> Parser::parseMatchingPattern(bool isExprBasic) {
  // TODO: Since we expect a pattern in this position, we should optimistically
  // parse pattern nodes for productions shared by pattern and expression
  // grammar. For short-term ease of initial implementation, we always go
  // through the expr parser for ambiguous productions.

  // Parse productions that can only be patterns.
  if (Tok.isAny(tok::kw_var, tok::kw_let)) {
    assert(Tok.isAny(tok::kw_let, tok::kw_var) && "expects var or let");
    bool isLet = Tok.is(tok::kw_let);
    SourceLoc varLoc = consumeToken();
    
    return parseMatchingPatternAsLetOrVar(isLet, varLoc, isExprBasic);
  }
  
  // matching-pattern ::= 'is' type
  if (Tok.is(tok::kw_is)) {
    SourceLoc isLoc = consumeToken(tok::kw_is);
    ParserResult<TypeRepr> castType = parseType();
    if (castType.isNull() || castType.hasCodeCompletion())
      return nullptr;
    return makeParserResult(new (Context) IsPattern(isLoc, castType.get(),
                                                    nullptr));
  }

  // matching-pattern ::= expr
  // Fall back to expression parsing for ambiguous forms. Name lookup will
  // disambiguate.
  ParserResult<Expr> subExpr =
    parseExprImpl(diag::expected_pattern, isExprBasic);
  if (subExpr.hasCodeCompletion())
    return makeParserCodeCompletionStatus();
  if (subExpr.isNull())
    return nullptr;
  
  // The most common case here is to parse something that was a lexically
  // obvious pattern, which will come back wrapped in an immediate
  // UnresolvedPatternExpr.  Transform this now to simplify later code.
  if (auto *UPE = dyn_cast<UnresolvedPatternExpr>(subExpr.get()))
    return makeParserResult(UPE->getSubPattern());
  
  return makeParserResult(new (Context) ExprPattern(subExpr.get()));
}

ParserResult<Pattern> Parser::parseMatchingPatternAsLetOrVar(bool isLet,
                                                             SourceLoc varLoc,
                                                             bool isExprBasic) {
  // 'var' and 'let' patterns shouldn't nest.
  if (InVarOrLetPattern == IVOLP_InLet ||
      InVarOrLetPattern == IVOLP_InVar)
    diagnose(varLoc, diag::var_pattern_in_var, unsigned(isLet));
  
  // 'let' isn't valid inside an implicitly immutable context, but var is.
  if (isLet && InVarOrLetPattern == IVOLP_ImplicitlyImmutable)
    diagnose(varLoc, diag::let_pattern_in_immutable_context);

  if (!isLet && InVarOrLetPattern == IVOLP_AlwaysImmutable)
    diagnose(varLoc, diag::var_not_allowed_in_pattern, isLet)
      .fixItReplace(varLoc, "let");

  // In our recursive parse, remember that we're in a var/let pattern.
  llvm::SaveAndRestore<decltype(InVarOrLetPattern)>
    T(InVarOrLetPattern, isLet ? IVOLP_InLet : IVOLP_InVar);

  ParserResult<Pattern> subPattern = parseMatchingPattern(isExprBasic);
  if (subPattern.isNull())
    return nullptr;
  auto *varP = new (Context) VarPattern(varLoc, isLet, subPattern.get());
  return makeParserResult(varP);
}


bool Parser::isOnlyStartOfMatchingPattern() {
  return Tok.isAny(tok::kw_var, tok::kw_let, tok::kw_is);
}


static bool canParsePatternTuple(Parser &P);

///   pattern ::= identifier
///   pattern ::= '_'
///   pattern ::= pattern-tuple
///   pattern ::= 'var' pattern
///   pattern ::= 'let' pattern
static bool canParsePattern(Parser &P) {
  switch (P.Tok.getKind()) {
  case tok::identifier:
  case tok::kw__:
    P.consumeToken();
    return true;
  case tok::kw_let:
  case tok::kw_var:
    P.consumeToken();
    return canParsePattern(P);
  case tok::l_paren:
    return canParsePatternTuple(P);

  default:
    return false;
  }
}


static bool canParsePatternTuple(Parser &P) {
  if (!P.consumeIf(tok::l_paren)) return false;

  if (P.Tok.isNot(tok::r_paren)) {
    do {
      if (!canParsePattern(P)) return false;
    } while (P.consumeIf(tok::comma));
  }

  return P.consumeIf(tok::r_paren);
}

///  typed-pattern ::= pattern (':' type)?
///
bool Parser::canParseTypedPattern() {
  if (!canParsePattern(*this)) return false;
  
  if (consumeIf(tok::colon))
    return canParseType();
  return true;
}


