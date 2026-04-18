import ts from "typescript";

import { kindName } from "./oracle.js";

/*
 * 把 ts 的 SourceFile 序列化为和 ctsc/src/parser/ast_json.c 字段一致的 JSON。
 *
 * 每个 kind 只输出 ctsc 目前已经发射的字段，让 byte-equal 比较能稳步推进。
 * 当 ctsc 增加一个新节点字段时，这里也要同步扩展。
 */

interface Out { kind: string; pos: number; end: number; [k: string]: unknown; }

function emit(node: ts.Node): Out {
  const o: Out = { kind: kindName(node.kind), pos: node.pos, end: node.end };
  switch (node.kind) {
    case ts.SyntaxKind.SourceFile: {
      const sf = node as ts.SourceFile;
      o.statements = sf.statements.map(emit);
      return o;
    }
    case ts.SyntaxKind.Block: {
      const b = node as ts.Block;
      o.statements = b.statements.map(emit);
      return o;
    }
    case ts.SyntaxKind.Identifier: {
      o.escapedText = (node as ts.Identifier).escapedText as string;
      return o;
    }
    case ts.SyntaxKind.NumericLiteral: {
      o.text = (node as ts.NumericLiteral).text;
      return o;
    }
    case ts.SyntaxKind.StringLiteral: {
      const lit = node as ts.StringLiteral & { singleQuote?: boolean };
      o.text = lit.text;
      if (lit.singleQuote) o.singleQuote = true;
      return o;
    }
    case ts.SyntaxKind.ExpressionStatement: {
      o.expression = emit((node as ts.ExpressionStatement).expression);
      return o;
    }
    case ts.SyntaxKind.EmptyStatement:
      return o;
    case ts.SyntaxKind.ReturnStatement: {
      const r = node as ts.ReturnStatement;
      if (r.expression) o.expression = emit(r.expression);
      return o;
    }
    case ts.SyntaxKind.VariableStatement: {
      const vs = node as ts.VariableStatement;
      o.declarationList = emit(vs.declarationList);
      return o;
    }
    case ts.SyntaxKind.VariableDeclarationList: {
      const l = node as ts.VariableDeclarationList;
      o.declarations = l.declarations.map(emit);
      return o;
    }
    case ts.SyntaxKind.VariableDeclaration: {
      const d = node as ts.VariableDeclaration;
      o.name = emit(d.name);
      if (d.initializer) o.initializer = emit(d.initializer);
      return o;
    }
    case ts.SyntaxKind.BinaryExpression: {
      const b = node as ts.BinaryExpression;
      o.left = emit(b.left);
      o.operatorToken = { kind: kindName(b.operatorToken.kind), pos: b.operatorToken.pos, end: b.operatorToken.end };
      o.right = emit(b.right);
      return o;
    }
    case ts.SyntaxKind.CallExpression: {
      const c = node as ts.CallExpression;
      o.expression = emit(c.expression);
      o.arguments = c.arguments.map(emit);
      return o;
    }
    case ts.SyntaxKind.FunctionDeclaration: {
      const f = node as ts.FunctionDeclaration;
      if (f.name) o.name = emit(f.name);
      o.parameters = f.parameters.map(emit);
      if (f.body) o.body = emit(f.body);
      return o;
    }
    case ts.SyntaxKind.Parameter: {
      const p = node as ts.ParameterDeclaration;
      o.name = emit(p.name);
      return o;
    }
    case ts.SyntaxKind.IfStatement: {
      const s = node as ts.IfStatement;
      o.expression = emit(s.expression);
      o.thenStatement = emit(s.thenStatement);
      if (s.elseStatement) o.elseStatement = emit(s.elseStatement);
      return o;
    }
    case ts.SyntaxKind.WhileStatement: {
      const s = node as ts.WhileStatement;
      o.expression = emit(s.expression);
      o.statement = emit(s.statement);
      return o;
    }
    case ts.SyntaxKind.ForStatement: {
      const s = node as ts.ForStatement;
      if (s.initializer) o.initializer = emit(s.initializer);
      if (s.condition) o.condition = emit(s.condition);
      if (s.incrementor) o.incrementor = emit(s.incrementor);
      o.statement = emit(s.statement);
      return o;
    }
    case ts.SyntaxKind.ParenthesizedExpression: {
      o.expression = emit((node as ts.ParenthesizedExpression).expression);
      return o;
    }
    case ts.SyntaxKind.PrefixUnaryExpression: {
      const u = node as ts.PrefixUnaryExpression;
      // Convert numeric operator to canonical kind name for cross-enum parity.
      o.operator = kindName(u.operator);
      o.operand = emit(u.operand);
      return o;
    }
    case ts.SyntaxKind.PropertyAccessExpression: {
      const pa = node as ts.PropertyAccessExpression;
      o.expression = emit(pa.expression);
      o.name = emit(pa.name);
      return o;
    }
    case ts.SyntaxKind.ConditionalExpression: {
      const c = node as ts.ConditionalExpression;
      o.condition = emit(c.condition);
      o.whenTrue = emit(c.whenTrue);
      o.whenFalse = emit(c.whenFalse);
      return o;
    }
    default: {
      const children: Out[] = [];
      ts.forEachChild(node, (c) => { children.push(emit(c)); });
      if (children.length) o.children = children;
      return o;
    }
  }
}

export function buildAstJson(src: string, fileName = "input.ts"): string {
  const sf = ts.createSourceFile(fileName, src, { languageVersion: ts.ScriptTarget.Latest }, /*setParentNodes*/ false);
  return JSON.stringify(emit(sf));
}
