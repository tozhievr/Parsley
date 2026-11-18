module;
#include <string>
#include <string_view>
#include <vector>
#include <optional>

export module parsley.stdlib;

export namespace parsley::stdlib {

struct StdModule {
    std::string name;
    std::string description;
    std::vector<std::string> exported_rules;
    std::vector<std::string> exported_types;
    std::string source;
};

const std::vector<StdModule>& builtin_modules() {
    static const std::vector<StdModule> modules = {
        StdModule{
            .name = "Parsley.Prelude",
            .description = "Core combinators, repetition, lookahead, and control flow primitives",
            .exported_rules = {
                "sepBy",
                "sepEndBy",
                "many",
                "some",
                "optional",
                "between",
                "eof",
                "lookahead",
                "notFollowedBy",
                "recover",
                "exactly"
            },
            .exported_types = {
                "Rule"
            },
            .source = R"PARSLEY(module Parsley.Prelude
  ( sepBy
  , sepEndBy
  , many
  , some
  , optional
  , between
  , eof
  , lookahead
  , notFollowedBy
  , recover
  , exactly
  ) where

-- Built-in combinator definitions
sepBy :: Rule a -> Rule sep -> Rule [a]
sepBy p sep = (:) <$> p <*> many (sep *> p) <|> pure []

sepEndBy :: Rule a -> Rule sep -> Rule [a]
sepEndBy p sep = sepBy p sep <* optional sep

between :: Rule open -> Rule close -> Rule a -> Rule a
between open close p = open *> p <* close

many :: Rule a -> Rule [a]
some :: Rule a -> Rule [a]
optional :: Rule a -> Rule (Maybe a)
eof :: Rule ()
lookahead :: Rule a -> Rule a
notFollowedBy :: Rule a -> Rule ()
recover :: Rule a -> Rule sync -> Rule a
exactly :: Text -> Rule Text
)PARSLEY"
        },
        StdModule{
            .name = "Parsley.Unicode",
            .description = "Legacy ASCII character categories (use Parsley.Ascii; not Unicode-aware)",
            .exported_rules = {
                "alpha",
                "digit",
                "hexDigit",
                "space",
                "whitespace",
                "upper",
                "lower",
                "letter",
                "alphanum",
                "newline"
            },
            .exported_types = {},
            .source = R"PARSLEY(module Parsley.Unicode
  ( alpha
  , digit
  , hexDigit
  , space
  , whitespace
  , upper
  , lower
  , letter
  , alphanum
  , newline
  ) where

token alpha      = /[a-zA-Z]/
token digit      = /[0-9]/
token hexDigit   = /[0-9a-fA-F]/
token space      = /[ \t]/
token whitespace = /[ \t\r\n]/
token upper      = /[A-Z]/
token lower      = /[a-z]/
token letter     = /[a-zA-Z_]/
token alphanum   = /[a-zA-Z0-9_]/
token newline    = /\r?\n/
)PARSLEY"
        },
        StdModule{
            .name = "Parsley.Ascii",
            .description = "ASCII character categories, whitespace, and letter definitions",
            .exported_rules = {
                "alpha",
                "digit",
                "hexDigit",
                "space",
                "whitespace",
                "upper",
                "lower",
                "letter",
                "alphanum",
                "newline"
            },
            .exported_types = {},
            .source = R"PARSLEY(module Parsley.Ascii
  ( alpha
  , digit
  , hexDigit
  , space
  , whitespace
  , upper
  , lower
  , letter
  , alphanum
  , newline
  ) where

token alpha      = /[a-zA-Z]/
token digit      = /[0-9]/
token hexDigit   = /[0-9a-fA-F]/
token space      = /[ \t]/
token whitespace = /[ \t\r\n]/
token upper      = /[A-Z]/
token lower      = /[a-z]/
token letter     = /[a-zA-Z_]/
token alphanum   = /[a-zA-Z0-9_]/
token newline    = /\r?\n/
)PARSLEY"
        },
        StdModule{
            .name = "Parsley.Layout",
            .description = "Layout and indentation-sensitive parsing combinators (offside rule)",
            .exported_rules = {
                "indent",
                "dedent",
                "newline",
                "sameIndent",
                "block"
            },
            .exported_types = {},
            .source = R"PARSLEY(module Parsley.Layout
  ( indent
  , dedent
  , newline
  , sameIndent
  , block
  ) where

import Parsley.Prelude (many)

token indent     = /<INDENT>/
token dedent     = /<DEDENT>/
token newline    = /<NEWLINE>/
token sameIndent = /<SAME_INDENT>/

block :: Rule a -> Rule [a]
block p = indent *> many p <* dedent
)PARSLEY"
        },
        StdModule{
            .name = "Json",
            .description = "Standard JSON grammar according to RFC 8259",
            .exported_rules = {
                "value",
                "object",
                "array",
                "string",
                "number",
                "boolean",
                "null"
            },
            .exported_types = {
                "JsonValue"
            },
            .source = R"PARSLEY(module Json (value, object, array, string, number, boolean, null) where

import Parsley.Prelude (sepBy, eof)

skip ws = /[[:space:]]+/

token numberLit = /-?(0|[1-9][0-9]*)([.][0-9]+)?([eE][+-]?[0-9]+)?/ :: Double
token stringLit = /"([^"\\]|\\.)*"/                                    :: Text

keywords = ["true", "false", "null"]

data JsonValue
  = JObject [(Text, JsonValue)]
  | JArray [JsonValue]
  | JString Text
  | JNumber Double
  | JBool Bool
  | JNull

value :: JsonValue
value
  =  JObject <$> object
  |  JArray  <$> array
  |  JString <$> string
  |  JNumber <$> number
  |  JBool   <$> boolean
  |  "null"  *>  pure JNull

object :: [(Text, JsonValue)]
object = "{" *> (member `sepBy` ",") <* "}"

member :: (Text, JsonValue)
member = do
  k <- string
  ":"
  v <- value
  pure (k, v)

array :: [JsonValue]
array = "[" *> (value `sepBy` ",") <* "]"

string :: Text
string = stringLit

number :: Double
number = numberLit

boolean :: Bool
boolean = ("true" *> pure True) <|> ("false" *> pure False)
)PARSLEY"
        },
        StdModule{
            .name = "Sql",
            .description = "SQL SELECT, INSERT, UPDATE, DELETE grammar subset",
            .exported_rules = {
                "statement",
                "selectStmt",
                "insertStmt",
                "updateStmt",
                "deleteStmt",
                "expr"
            },
            .exported_types = {
                "SqlStmt",
                "SqlExpr"
            },
            .source = R"PARSLEY(module Sql (statement, selectStmt, insertStmt, updateStmt, deleteStmt, expr) where

import Parsley.Prelude (sepBy, optional, eof)

skip ws = /[[:space:]]+/

token ident     = /[a-zA-Z_][a-zA-Z0-9_]*/ :: Text
token numberLit = /[0-9]+([.][0-9]+)?/     :: Double
token stringLit = /'([^']|'')*'/           :: Text

keywords = ["SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE", "SET", "DELETE"]

data SqlStmt
  = Select [Text] Text (Maybe SqlExpr)
  | Insert Text [Text] [SqlExpr]
  | Update Text [(Text, SqlExpr)] (Maybe SqlExpr)
  | Delete Text (Maybe SqlExpr)

data SqlExpr
  = Col Text
  | LitNum Double
  | LitStr Text

statement :: SqlStmt
statement
  =  selectStmt
  |  insertStmt
  |  updateStmt
  |  deleteStmt

selectStmt :: SqlStmt
selectStmt = do
  "SELECT"
  cols <- ident `sepBy` ","
  "FROM"
  tbl <- ident
  cond <- optional ("WHERE" *> expr)
  pure (Select cols tbl cond)

insertStmt :: SqlStmt
insertStmt = do
  "INSERT"
  "INTO"
  tbl <- ident
  cols <- "(" *> (ident `sepBy` ",") <* ")"
  "VALUES"
  vals <- "(" *> (expr `sepBy` ",") <* ")"
  pure (Insert tbl cols vals)

updateStmt :: SqlStmt
updateStmt = do
  "UPDATE"
  tbl <- ident
  "SET"
  assignments <- (do c <- ident; "="; e <- expr; pure (c, e)) `sepBy` ","
  cond <- optional ("WHERE" *> expr)
  pure (Update tbl assignments cond)

deleteStmt :: SqlStmt
deleteStmt = do
  "DELETE"
  "FROM"
  tbl <- ident
  cond <- optional ("WHERE" *> expr)
  pure (Delete tbl cond)

expr :: SqlExpr
expr
  =  Col    <$> ident
  |  LitNum <$> numberLit
  |  LitStr <$> stringLit
)PARSLEY"
        },
        StdModule{
            .name = "Python",
            .description = "Full Python 3 statement, expression, and AST grammar derived from ANTLR4 Python3",
            .exported_rules = {"moduleFile"},
            .exported_types = {"PyOp", "PyUnaryOp", "PyParam", "PyExpr", "PyStmt", "PyModule"},
            .source = R"PARSLEY({-# MODE PEG #-}
module Python (moduleFile) where

import Parsley.Prelude (sepBy, many, some, optional, eof)

-- Лексика Python 3 (на базе PythonLexer.g4) -------------------------
skip ws      = /[[:space:]]+/
skip comment = /#[^\n]*/

token numberLit = /[0-9]+([.][0-9]+)?/                   :: Double
token stringLit = /"([^"\\]|\\.)*"|'([^'\\]|\\.)*'/      :: Text
token ident     = /[a-zA-Z_][a-zA-Z0-9_]*/               :: Text

keywords =
  [ "import", "from", "as", "def", "class", "if", "elif", "else"
  , "while", "for", "in", "return", "pass", "break", "continue"
  , "and", "or", "not", "True", "False", "None"
  ]

-- AST (транслируется в ISO C23 структуры) ---------------------------
data PyOp
  = PyAdd | PySub | PyMul | PyDiv | PyFloorDiv | PyMod | PyPow
  | PyEq | PyNe | PyLt | PyLe | PyGt | PyGe
  | PyAnd | PyOr

data PyUnaryOp
  = PyNeg | PyPos | PyNot

data PyParam
  = PyParam Text Text

data PyExpr
  = PyNum Double
  | PyStr Text
  | PyBool Bool
  | PyNone
  | PyVar Text
  | PyList [PyExpr]
  | PyBin PyOp PyExpr PyExpr
  | PyUnary PyUnaryOp PyExpr
  | PyCall PyExpr [PyExpr]
  | PyIndex PyExpr PyExpr
  | PyAttr PyExpr Text

data PyStmt
  = PyImport [Text] Text
  | PyFromImport [Text] [Text]
  | PyDef Text [PyParam] Text [PyStmt]
  | PyClass Text [Text] [PyStmt]
  | PyIf PyExpr [PyStmt] [PyStmt]
  | PyWhile PyExpr [PyStmt]
  | PyFor Text PyExpr [PyStmt]
  | PyReturn PyExpr
  | PyPass
  | PyBreak
  | PyContinue
  | PyAssign PyExpr Text PyExpr
  | PyAugAssign PyExpr Text PyExpr
  | PyExprStmt PyExpr

data PyModule
  = PyModule [PyStmt]

-- Точка входа -------------------------------------------------------
moduleFile :: PyModule
moduleFile = PyModule <$> (many topStmt <* eof)

topStmt :: PyStmt
topStmt
  =  importStmt
  |  fromImportStmt
  |  defStmt
  |  classStmt
  |  innerStmt

-- Импорты -----------------------------------------------------------
dottedName :: [Text]
dottedName = ident `sepBy` "."

optAlias :: Text
optAlias
  =  "as" *> ident
  |  pure ""

importStmt :: PyStmt
importStmt = do
  "import"
  modPath <- dottedName
  alias   <- optAlias
  pure (PyImport modPath alias)

fromTargets :: [Text]
fromTargets
  =  "*" *> pure ["*"]
  |  "(" *> (ident `sepBy` ",") <* ")"
  |  ident `sepBy` ","

fromImportStmt :: PyStmt
fromImportStmt = do
  "from"
  modPath <- dottedName
  "import"
  names   <- fromTargets
  pure (PyFromImport modPath names)

-- Определения функций и классов -------------------------------------
typeName :: Text
typeName = do
  base <- ("None" *> pure "None") <|> ident
  optional ("[" *> (typeName `sepBy` ",") <* "]")
  pure base

optAnnot :: Text
optAnnot
  =  ":" *> typeName
  |  pure ""

optReturnType :: Text
optReturnType
  =  "->" *> typeName
  |  pure ""

param :: PyParam
param = do
  name <- ident
  ann  <- optAnnot
  optional ("=" *> expr)
  pure (PyParam name ann)

defStmt :: PyStmt
defStmt = do
  "def"
  name   <- ident
  "("
  params <- param `sepBy` ","
  ")"
  ret    <- optReturnType
  ":"
  body   <- suite
  pure (PyDef name params ret body)

optBases :: [Text]
optBases
  =  "(" *> (ident `sepBy` ",") <* ")"
  |  pure []

classStmt :: PyStmt
classStmt = do
  "class"
  name  <- ident
  bases <- optBases
  ":"
  body  <- classSuite
  pure (PyClass name bases body)

classSuite :: [PyStmt]
classSuite = some classMember

classMember :: PyStmt
classMember
  =  defStmt
  |  innerStmt

-- Операторы и блоки -------------------------------------------------
suite :: [PyStmt]
suite = some innerStmt

innerStmt :: PyStmt
innerStmt
  =  ifStmt
  |  whileStmt
  |  forStmt
  |  returnStmt
  |  passStmt
  |  breakStmt
  |  continueStmt
  |  annAssignStmt
  |  augAssignStmt
  |  plainAssignStmt
  |  exprStmt

ifStmt :: PyStmt
ifStmt = do
  "if"
  cond <- expr
  ":"
  body <- suite
  rest <- elifOrElse
  pure (PyIf cond body rest)

elifOrElse :: [PyStmt]
elifOrElse
  =  elifBranch
  |  "else" *> ":" *> suite
  |  pure []

elifBranch :: [PyStmt]
elifBranch = do
  "elif"
  cond <- expr
  ":"
  body <- suite
  rest <- elifOrElse
  pure [PyIf cond body rest]

whileStmt :: PyStmt
whileStmt = do
  "while"
  cond <- expr
  ":"
  body <- suite
  pure (PyWhile cond body)

forStmt :: PyStmt
forStmt = do
  "for"
  var  <- ident
  "in"
  iter <- expr
  ":"
  body <- suite
  pure (PyFor var iter body)

returnStmt :: PyStmt
returnStmt
  =  "return" *> (PyReturn <$> expr)
  |  "return" *> pure (PyReturn PyNone)

passStmt :: PyStmt
passStmt = "pass" *> pure PyPass

breakStmt :: PyStmt
breakStmt = "break" *> pure PyBreak

continueStmt :: PyStmt
continueStmt = "continue" *> pure PyContinue

annAssignStmt :: PyStmt
annAssignStmt = do
  name <- ident
  ":"
  ann  <- typeName
  "="
  val  <- expr
  pure (PyAssign (PyVar name) ann val)

augOp :: Text
augOp
  =  "+="  *> pure "+="
  |  "-="  *> pure "-="
  |  "*="  *> pure "*="
  |  "/="  *> pure "/="
  |  "//=" *> pure "//="
  |  "%="  *> pure "%="

augAssignStmt :: PyStmt
augAssignStmt = do
  target <- postfixExpr
  op     <- augOp
  val    <- expr
  pure (PyAugAssign target op val)

plainAssignStmt :: PyStmt
plainAssignStmt = do
  target <- postfixExpr
  "="
  val    <- expr
  pure (PyAssign target "" val)

exprStmt :: PyStmt
exprStmt = PyExprStmt <$> expr

-- Выражения (приоритеты операторов Python 3) ------------------------
expr :: PyExpr
expr = operators postfixExpr
  [ infixl 1 ["or"  => PyBin PyOr]
  , infixl 2 ["and" => PyBin PyAnd]
  , prefix 3 ["not" => PyUnary PyNot]
  , infixl 4 ["==" => PyBin PyEq, "!=" => PyBin PyNe, "<=" => PyBin PyLe, "<" => PyBin PyLt, ">=" => PyBin PyGe, ">" => PyBin PyGt]
  , infixl 5 ["+" => PyBin PyAdd, "-" => PyBin PySub]
  , infixl 6 ["*" => PyBin PyMul, "//" => PyBin PyFloorDiv, "/" => PyBin PyDiv, "%" => PyBin PyMod]
  , prefix 7 ["-" => PyUnary PyNeg, "+" => PyUnary PyPos]
  , infixr 8 ["**" => PyBin PyPow]
  ]

postfixExpr :: PyExpr
postfixExpr
  =  PyCall  <$> postfixExpr <*> ("(" *> (expr `sepBy` ",") <* ")")
  |  PyIndex <$> postfixExpr <*> ("[" *> expr <* "]")
  |  PyAttr  <$> postfixExpr <*> ("." *> ident)
  |  atom

atom :: PyExpr
atom
  =  PyNum  <$> numberLit
  |  PyStr  <$> stringLit
  |  "True"  *> pure (PyBool True)
  |  "False" *> pure (PyBool False)
  |  "None"  *> pure PyNone
  |  PyList <$> ("[" *> (expr `sepBy` ",") <* "]")
  |  PyVar  <$> ident
  |  "(" *> expr <* ")"

-- Примеры и тесты ---------------------------------------------------
example "arithmetic" : "x = 1 + 2 * 3" parses-to PyModule [PyAssign (PyVar "x") "" (PyBin PyAdd (PyNum 1) (PyBin PyMul (PyNum 2) (PyNum 3)))]
example "import" : "from typing import List, Optional" parses-to PyModule [PyFromImport ["typing"] ["List", "Optional"]]
example "function" : "def add(x: int, y: int) -> int: return x + y" parses-to PyModule [PyDef "add" [PyParam "x" "int", PyParam "y" "int"] "int" [PyReturn (PyBin PyAdd (PyVar "x") (PyVar "y"))]]
example "if-elif-else" : "if x > 0: pass elif x == 0: break else: continue" parses-to PyModule [PyIf (PyBin PyGt (PyVar "x") (PyNum 0)) [PyPass] [PyIf (PyBin PyEq (PyVar "x") (PyNum 0)) [PyBreak] [PyContinue]]]
example "unary-and-pow" : "y = -2 ** 3" parses-to PyModule [PyAssign (PyVar "y") "" (PyUnary PyNeg (PyBin PyPow (PyNum 2) (PyNum 3)))]
example "error-missing-colon" : "def broken(x)" fails-at (1, 14) expecting ":"
example "error-bad-expr" : "x = 1 +" fails-at (1, 8) expecting "expression"
)PARSLEY"
        },
        StdModule{
            .name = "Swift",
            .description = "Swift 5 declarations, statements, and expression grammar derived from ANTLR4 Swift5",
            .exported_rules = {"topLevel"},
            .exported_types = {"SwiftOp", "SwiftUnaryOp", "SwiftExpr", "SwiftParam", "SwiftField", "SwiftStmt", "SwiftDecl", "SwiftFile"},
            .source = R"PARSLEY(module Swift (topLevel) where

import Parsley.Prelude (sepBy, many, eof)

-- Лексика (по мотивам Swift5Lexer.g4) ----------------
skip ws           = /[[:space:]]+/
skip lineComment  = /\/\/[^\n\r]*/
skip blockComment = /\/\*([^*]|\*+[^*/])*\*+\//

token number    = /[0-9]+([.][0-9]+)?/   :: Double
token stringLit = /"([^"\\]|\\.)*"/      :: Text
token ident     = /[a-zA-Z_][a-zA-Z0-9_]*/ :: Text

keywords =
  [ "import", "struct", "class", "enum", "case", "func"
  , "let", "var", "if", "else", "guard", "while", "for"
  , "in", "return", "true", "false", "nil"
  ]

-- AST (Swift 5 subset) ---------------------------------
data SwiftOp
  = SwAdd | SwSub | SwMul | SwDiv | SwMod
  | SwEq  | SwNeq | SwLt  | SwLe  | SwGt | SwGe
  | SwAnd | SwOr  | SwAssign

data SwiftUnaryOp = SwNeg | SwNot

data SwiftExpr
  = SwNum Double
  | SwStr Text
  | SwBool Bool
  | SwNil
  | SwVar Text
  | SwUnary SwiftUnaryOp SwiftExpr
  | SwBin SwiftOp SwiftExpr SwiftExpr
  | SwCall SwiftExpr [SwiftExpr]
  | SwMember SwiftExpr Text
  | SwArray [SwiftExpr]

data SwiftParam = SwParam Text Text

data SwiftField = SwField Bool Text Text [SwiftExpr]

data SwiftStmt
  = StmtLet Text Text SwiftExpr
  | StmtVar Text Text SwiftExpr
  | StmtIf SwiftExpr [SwiftStmt] [SwiftStmt]
  | StmtGuard SwiftExpr [SwiftStmt]
  | StmtWhile SwiftExpr [SwiftStmt]
  | StmtFor Text SwiftExpr [SwiftStmt]
  | StmtReturn [SwiftExpr]
  | StmtExpr SwiftExpr

data SwiftDecl
  = DeclImport Text
  | DeclStruct Text [SwiftDecl]
  | DeclClass Text Text [SwiftDecl]
  | DeclEnum Text [SwiftDecl]
  | DeclEnumCase [Text]
  | DeclFunc Text [SwiftParam] Text [SwiftStmt]
  | DeclField SwiftField
  | DeclStmt SwiftStmt

data SwiftFile = SwFile [SwiftDecl]

-- Синтаксис --------------------------------------------
topLevel :: SwiftFile
topLevel = SwFile <$> many topDecl <* many ";" <* eof

topDecl :: SwiftDecl
topDecl
  =  importDecl
  |  structDecl
  |  classDecl
  |  enumDecl
  |  funcDecl
  |  DeclStmt <$> stmt

importDecl :: SwiftDecl
importDecl = DeclImport <$> ("import" *> ident <* many ";")

structDecl :: SwiftDecl
structDecl = do
  "struct"
  name <- ident
  "{"
  members <- many memberDecl
  "}"
  many ";"
  pure (DeclStruct name members)

classDecl :: SwiftDecl
classDecl = do
  "class"
  name <- ident
  super <- optSuper
  "{"
  members <- many memberDecl
  "}"
  many ";"
  pure (DeclClass name super members)

optSuper :: Text
optSuper = (":" *> typeRef) <|> pure ""

enumDecl :: SwiftDecl
enumDecl = do
  "enum"
  name <- ident
  "{"
  members <- many enumMember
  "}"
  many ";"
  pure (DeclEnum name members)

enumMember :: SwiftDecl
enumMember
  =  enumCaseDecl
  |  memberDecl

enumCaseDecl :: SwiftDecl
enumCaseDecl = DeclEnumCase <$> ("case" *> (enumCaseName `sepBy` ",") <* many ";")

enumCaseName :: Text
enumCaseName = ident <* optAssocParams

optAssocParams :: [Text]
optAssocParams = parens (typeRef `sepBy` ",") <|> pure []

memberDecl :: SwiftDecl
memberDecl
  =  DeclField <$> fieldDecl
  |  funcDecl
  |  structDecl
  |  classDecl
  |  enumDecl

fieldDecl :: SwiftField
fieldDecl
  =  letField
  |  varField

letField :: SwiftField
letField = do
  "let"
  name <- ident
  ty   <- optTypeAnn
  init <- optInit
  many ";"
  pure (SwField False name ty init)

varField :: SwiftField
varField = do
  "var"
  name <- ident
  ty   <- optTypeAnn
  init <- optInit
  many ";"
  pure (SwField True name ty init)

optInit :: [SwiftExpr]
optInit = ("=" *> singleExprList) <|> pure []

singleExprList :: [SwiftExpr]
singleExprList = do
  e <- expr
  pure [e]

funcDecl :: SwiftDecl
funcDecl = do
  "func"
  name   <- ident
  params <- parens (funcParam `sepBy` ",")
  ret    <- optRetType
  body   <- block
  many ";"
  pure (DeclFunc name params ret body)

funcParam :: SwiftParam
funcParam = do
  name <- ("_" *> ident) <|> ident
  ":"
  ty   <- typeRef
  pure (SwParam name ty)

optTypeAnn :: Text
optTypeAnn = (":" *> typeRef) <|> pure ""

optRetType :: Text
optRetType = ("->" *> typeRef) <|> pure ""

typeRef :: Text
typeRef
  =  ("[" *> typeRef <* "]")
  |  ident

block :: [SwiftStmt]
block = "{" *> many stmt <* "}"

stmt :: SwiftStmt
stmt
  =  letStmt
  |  varStmt
  |  ifStmt
  |  guardStmt
  |  whileStmt
  |  forStmt
  |  returnStmt
  |  StmtExpr <$> (expr <* many ";")

letStmt :: SwiftStmt
letStmt = do
  "let"
  name <- ident
  ty   <- optTypeAnn
  "="
  val  <- expr
  many ";"
  pure (StmtLet name ty val)

varStmt :: SwiftStmt
varStmt = do
  "var"
  name <- ident
  ty   <- optTypeAnn
  "="
  val  <- expr
  many ";"
  pure (StmtVar name ty val)

ifStmt :: SwiftStmt
ifStmt = do
  "if"
  cond     <- expr
  thenBody <- block
  elseBody <- elseClause <|> pure []
  many ";"
  pure (StmtIf cond thenBody elseBody)

elseClause :: [SwiftStmt]
elseClause = "else" *> (block <|> elseIfList)

elseIfList :: [SwiftStmt]
elseIfList = do
  s <- ifStmt
  pure [s]

guardStmt :: SwiftStmt
guardStmt = do
  "guard"
  cond     <- expr
  "else"
  elseBody <- block
  many ";"
  pure (StmtGuard cond elseBody)

whileStmt :: SwiftStmt
whileStmt = do
  "while"
  cond <- expr
  body <- block
  many ";"
  pure (StmtWhile cond body)

forStmt :: SwiftStmt
forStmt = do
  "for"
  name <- ident
  "in"
  iter <- expr
  body <- block
  many ";"
  pure (StmtFor name iter body)

returnStmt :: SwiftStmt
returnStmt = do
  "return"
  val <- singleExprList <|> pure []
  many ";"
  pure (StmtReturn val)

expr :: SwiftExpr
expr = operators atom
  [ infixr 1 ["="  => SwBin SwAssign]
  , infixl 2 ["||" => SwBin SwOr]
  , infixl 3 ["&&" => SwBin SwAnd]
  , infixl 4 ["==" => SwBin SwEq, "!=" => SwBin SwNeq, "<=" => SwBin SwLe, ">=" => SwBin SwGe, "<" => SwBin SwLt, ">" => SwBin SwGt]
  , infixl 6 ["+"  => SwBin SwAdd, "-" => SwBin SwSub]
  , infixl 7 ["*"  => SwBin SwMul, "/" => SwBin SwDiv, "%" => SwBin SwMod]
  , prefix 9 ["-"  => SwUnary SwNeg, "!" => SwUnary SwNot]
  ]

atom :: SwiftExpr
atom
  =  SwNum  <$> number
  |  SwStr  <$> stringLit
  |  ("true"  *> pure (SwBool True))
  |  ("false" *> pure (SwBool False))
  |  ("nil"   *> pure SwNil)
  |  SwArray <$> ("[" *> (expr `sepBy` ",") <* "]")
  |  parens expr
  |  identAtom

identAtom :: SwiftExpr
identAtom
  =  SwCall   <$> (SwVar <$> ident) <*> parens (expr `sepBy` ",")
  |  SwMember <$> (SwVar <$> ident) <*> ("." *> ident)
  |  SwVar    <$> ident

parens :: Rule a -> Rule a
parens p = "(" *> p <* ")"

-- Примеры и тесты --------------------------------------
example "func_add" : "func add(a: Int, b: Int) -> Int { return a + b }" parses-to SwFile [DeclFunc "add" [SwParam "a" "Int", SwParam "b" "Int"] "Int" [StmtReturn [SwBin SwAdd (SwVar "a") (SwVar "b")]]]
example "let_var" : "let x: Int = 10\nvar y = x * 2" parses-to SwFile [DeclStmt (StmtLet "x" "Int" (SwNum 10)), DeclStmt (StmtVar "y" "" (SwBin SwMul (SwVar "x") (SwNum 2)))]
example "guard_if" : "guard x > 0 else { return 0 }" parses-to SwFile [DeclStmt (StmtGuard (SwBin SwGt (SwVar "x") (SwNum 0)) [StmtReturn [SwNum 0]])]
example "struct_enum" : "struct Point { let x: Double\n var y: Double }" parses-to SwFile [DeclStruct "Point" [DeclField (SwField False "x" "Double" []), DeclField (SwField True "y" "Double" [])]]
example "syntax_err" : "func f( {" fails-at (1, 9) expecting "ident"
)PARSLEY"
        },
        StdModule{
            .name = "Kotlin",
            .description = "Kotlin declarations, statements, expressions, and when branches derived from ANTLR4 Kotlin",
            .exported_rules = {"kotlinFile"},
            .exported_types = {"KtOp", "KtUnaryOp", "KtExpr", "KtWhenBranch", "KtStmt", "KtParam", "KtDecl", "KtFile"},
            .source = R"PARSLEY(module Kotlin (kotlinFile) where

import Parsley.Prelude (sepBy, many, eof)

-- Лексика (derived from ANTLR4 KotlinLexer.g4) -----------------
skip ws           = /[[:space:]]+/
skip lineComment  = /\/\/[^\n]*/
skip blockComment = /\/\*([^*]|\*+[^*\/])*\*+\//

token floatLit = /[0-9]+[.][0-9]+/          :: Double
token intLit   = /[0-9]+/                   :: Double
token strLit   = /"([^"\\]|\\.)*"/          :: Text
token ident    = /[a-zA-Z_][a-zA-Z0-9_]*/   :: Text

keywords =
  [ "package", "import", "as"
  , "class", "interface", "fun", "val", "var"
  , "if", "else", "when", "while", "for", "in", "return"
  , "true", "false", "null"
  , "data", "open", "abstract", "override", "private", "public", "internal"
  ]

-- AST (core Kotlin constructs for ISO C23 static analysis) -----
data KtOp
  = Or | And | Eq | Neq | Lt | Gt | Le | Ge | Range | Add | Sub | Mul | Div | Mod

data KtUnaryOp
  = Neg | Not

data KtExpr
  = IntLit Double
  | FloatLit Double
  | StrLit Text
  | BoolLit Bool
  | NullLit
  | Var Text
  | Bin KtOp KtExpr KtExpr
  | Unary KtUnaryOp KtExpr
  | Call Text [KtExpr]
  | Member KtExpr Text
  | MethodCall KtExpr Text [KtExpr]
  | Index KtExpr KtExpr
  | IfExpr KtExpr [KtStmt] [KtStmt]
  | WhenExpr [KtExpr] [KtWhenBranch]

data KtWhenBranch
  = WhenBranch [KtExpr] [KtStmt]
  | WhenElse [KtStmt]

data KtStmt
  = ValStmt Bool Text Text [KtExpr]
  | AssignStmt Text Text KtExpr
  | IfStmt KtExpr [KtStmt] [KtStmt]
  | WhenStmt [KtExpr] [KtWhenBranch]
  | WhileStmt KtExpr [KtStmt]
  | ForStmt Text KtExpr [KtStmt]
  | ReturnStmt [KtExpr]
  | ExprStmt KtExpr

data KtParam
  = Param Text Text Text

data KtDecl
  = ImportDecl [Text] Text
  | ClassDecl Text Text [KtParam] [KtDecl]
  | InterfaceDecl Text [KtDecl]
  | FunDecl Text [KtParam] Text [KtStmt]
  | PropDecl Bool Text Text [KtExpr]

data KtFile
  = KtFile [Text] [KtDecl]

-- Синтаксис (derived from ANTLR4 KotlinParser.g4) ---------------
kotlinFile :: KtFile
kotlinFile = do
  pkg <- packageHeader
  decls <- many topLevelDecl
  eof
  pure (KtFile pkg decls)

optSemi :: ()
optSemi
  =   ";" *> (do { pure () })
  <|> do { pure () }

packageHeader :: [Text]
packageHeader
  =   ("package" *> (ident `sepBy` ".") <* optSemi)
  <|> do { pure [] }

modifier :: ()
modifier
  =   "data" *> (do { pure () })
  <|> "open" *> (do { pure () })
  <|> "abstract" *> (do { pure () })
  <|> "override" *> (do { pure () })
  <|> "private" *> (do { pure () })
  <|> "public" *> (do { pure () })
  <|> "internal" *> (do { pure () })

classKind :: Text
classKind
  =   do
        "data"
        "class"
        pure "data"
  <|> do
        "open"
        "class"
        pure "open"
  <|> do
        "abstract"
        "class"
        pure "abstract"
  <|> do
        "private"
        "class"
        pure "private"
  <|> do
        "class"
        pure "class"

topLevelDecl :: KtDecl
topLevelDecl
  =   importDecl
  <|> classDecl
  <|> interfaceDecl
  <|> (many modifier *> (funDecl <|> propDecl))

importDecl :: KtDecl
importDecl = do
  "import"
  path <- ident `sepBy` "."
  alias <- importTail
  optSemi
  pure (ImportDecl path alias)

importTail :: Text
importTail
  =   do
        "."
        "*"
        pure "*"
  <|> "as" *> ident
  <|> do
        pure ""

classDecl :: KtDecl
classDecl = do
  kind <- classKind
  name <- ident
  params <- classParamList <|> (do { pure [] })
  _ <- superClause <|> (do { pure () })
  members <- classBody <|> (do { pure [] })
  optSemi
  pure (ClassDecl kind name params members)

superClause :: ()
superClause = do
  ":"
  _ <- ident
  _ <- optTypeArgs
  _ <- parens (expr `sepBy` ",") <|> (do { pure [] })
  pure ()

classParamList :: [KtParam]
classParamList = parens (classParam `sepBy` ",")

classParamMut :: Text
classParamMut
  =   do
        "val"
        pure "val"
  <|> do
        "var"
        pure "var"
  <|> do
        pure ""

classParam :: KtParam
classParam = do
  _ <- many modifier
  mut <- classParamMut
  name <- ident
  ":"
  ty <- typeExpr
  pure (Param mut name ty)

interfaceDecl :: KtDecl
interfaceDecl = do
  _ <- many modifier
  "interface"
  name <- ident
  members <- classBody <|> (do { pure [] })
  optSemi
  pure (InterfaceDecl name members)

classBody :: [KtDecl]
classBody = braces (many memberDecl)

memberDecl :: KtDecl
memberDecl = many modifier *> (funDecl <|> propDecl <|> classDecl <|> interfaceDecl)

funDecl :: KtDecl
funDecl = do
  "fun"
  name <- ident
  params <- parens (funParam `sepBy` ",")
  retTy <- optRetType
  body <- funBody
  optSemi
  pure (FunDecl name params retTy body)

funParam :: KtParam
funParam = do
  name <- ident
  ":"
  ty <- typeExpr
  pure (Param "" name ty)

optRetType :: Text
optRetType
  =   ":" *> typeExpr
  <|> do
        pure ""

funBody :: [KtStmt]
funBody
  =   block
  <|> do
        "="
        e <- expr
        pure [ReturnStmt [e]]
  <|> do
        pure []

isVarKw :: Bool
isVarKw
  =   do
        "var"
        pure True
  <|> do
        "val"
        pure False

propDecl :: KtDecl
propDecl = do
  isVar <- isVarKw
  name <- ident
  ty <- optRetType
  init <- optInit
  optSemi
  pure (PropDecl isVar name ty init)

optInit :: [KtExpr]
optInit
  =   do
        "="
        e <- expr
        pure [e]
  <|> do
        pure []

optTypeArgs :: ()
optTypeArgs
  =   do
        "<"
        _ <- typeExpr `sepBy` ","
        ">"
        pure ()
  <|> do
        pure ()

optQuest :: ()
optQuest
  =   do
        "?"
        pure ()
  <|> do
        pure ()

typeExpr :: Text
typeExpr = do
  base <- ident
  _ <- optTypeArgs
  _ <- optQuest
  pure base

block :: [KtStmt]
block = braces (many stmt)

blockOrSingleStmt :: [KtStmt]
blockOrSingleStmt
  =   block
  <|> do
        s <- stmt
        pure [s]

stmt :: KtStmt
stmt
  =   valStmt
  <|> ifStmt
  <|> whenStmt
  <|> whileStmt
  <|> forStmt
  <|> returnStmt
  <|> assignStmt
  <|> (ExprStmt <$> expr <* optSemi)

valStmt :: KtStmt
valStmt = do
  isVar <- isVarKw
  name <- ident
  ty <- optRetType
  init <- optInit
  optSemi
  pure (ValStmt isVar name ty init)

assignOp :: Text
assignOp
  =   do
        "="
        pure "="
  <|> do
        "+="
        pure "+="
  <|> do
        "-="
        pure "-="
  <|> do
        "*="
        pure "*="
  <|> do
        "/="
        pure "/="

assignStmt :: KtStmt
assignStmt = do
  target <- ident
  op <- assignOp
  rhs <- expr
  optSemi
  pure (AssignStmt target op rhs)

elseStmtBranch :: [KtStmt]
elseStmtBranch
  =   "else" *> blockOrSingleStmt
  <|> do
        pure []

ifStmt :: KtStmt
ifStmt = do
  "if"
  cond <- parens expr
  thenB <- blockOrSingleStmt
  elseB <- elseStmtBranch
  optSemi
  pure (IfStmt cond thenB elseB)

whenSubject :: [KtExpr]
whenSubject
  =   do
        e <- parens expr
        pure [e]
  <|> do
        pure []

whenStmt :: KtStmt
whenStmt = do
  "when"
  subj <- whenSubject
  branches <- braces (many whenBranch)
  optSemi
  pure (WhenStmt subj branches)

whenBranch :: KtWhenBranch
whenBranch
  =   do
        "else"
        "->"
        body <- blockOrSingleStmt
        optSemi
        pure (WhenElse body)
  <|> do
        conds <- expr `sepBy` ","
        "->"
        body <- blockOrSingleStmt
        optSemi
        pure (WhenBranch conds body)

whileStmt :: KtStmt
whileStmt = do
  "while"
  cond <- parens expr
  body <- blockOrSingleStmt
  optSemi
  pure (WhileStmt cond body)

forStmt :: KtStmt
forStmt = do
  "for"
  "("
  v <- ident
  "in"
  iter <- expr
  ")"
  body <- blockOrSingleStmt
  optSemi
  pure (ForStmt v iter body)

returnVal :: [KtExpr]
returnVal
  =   do
        e <- expr
        pure [e]
  <|> do
        pure []

returnStmt :: KtStmt
returnStmt = ReturnStmt <$> ("return" *> returnVal) <* optSemi

expr :: KtExpr
expr = operators atom
  [ infixl 1 ["||" => Bin Or]
  , infixl 2 ["&&" => Bin And]
  , infixl 3 ["==" => Bin Eq, "!=" => Bin Neq]
  , infixl 4 ["<=" => Bin Le, ">=" => Bin Ge, "<" => Bin Lt, ">" => Bin Gt]
  , infixl 5 [".." => Bin Range]
  , infixl 6 ["+" => Bin Add, "-" => Bin Sub]
  , infixl 7 ["*" => Bin Mul, "/" => Bin Div, "%" => Bin Mod]
  , prefix 8 ["-" => Unary Neg, "!" => Unary Not]
  ]

atom :: KtExpr
atom
  =   FloatLit <$> floatLit
  <|> IntLit <$> intLit
  <|> StrLit <$> strLit
  <|> ("true" *> pure (BoolLit True))
  <|> ("false" *> pure (BoolLit False))
  <|> ("null" *> pure NullLit)
  <|> ifExpr
  <|> whenExpr
  <|> do
        obj <- ident
        "."
        meth <- ident
        args <- parens (expr `sepBy` ",")
        pure (MethodCall (Var obj) meth args)
  <|> do
        obj <- ident
        "."
        fld <- ident
        pure (Member (Var obj) fld)
  <|> Call <$> ident <*> parens (expr `sepBy` ",")
  <|> do
        arr <- ident
        idx <- brackets expr
        pure (Index (Var arr) idx)
  <|> Var <$> ident
  <|> parens expr

ifExpr :: KtExpr
ifExpr = do
  "if"
  cond <- parens expr
  thenB <- blockOrSingleStmt
  "else"
  elseB <- blockOrSingleStmt
  pure (IfExpr cond thenB elseB)

whenExpr :: KtExpr
whenExpr = do
  "when"
  subj <- whenSubject
  branches <- braces (many whenBranch)
  pure (WhenExpr subj branches)

parens :: Rule a -> Rule a
parens p = "(" *> p <* ")"

brackets :: Rule a -> Rule a
brackets p = "[" *> p <* "]"

braces :: Rule a -> Rule a
braces p = "{" *> p <* "}"

-- Примеры и тесты ----------------------------------------------
example "fun_expr" : "fun sq(x: Int): Int = x * x" parses-to KtFile [] [FunDecl "sq" [Param "" "x" "Int"] "Int" [ReturnStmt [Bin Mul (Var "x") (Var "x")]]]

example "val_arith" : "fun main() { val x = 1 + 2 * 3 }" parses-to KtFile [] [FunDecl "main" [] "" [ValStmt False "x" "" [Bin Add (IntLit 1) (Bin Mul (IntLit 2) (IntLit 3))]]]

example "when_stmt" : "fun f(x: Int): Int { when (x) { 1 -> return 10; else -> return 0 } }" parses-to KtFile [] [FunDecl "f" [Param "" "x" "Int"] "Int" [WhenStmt [Var "x"] [WhenBranch [IntLit 1] [ReturnStmt [IntLit 10]], WhenElse [ReturnStmt [IntLit 0]]]]]

example "class_and_for" : "class Counter(var n: Int) { fun inc() { for (i in 1..5) { n += i } } }" parses-to KtFile [] [ClassDecl "class" "Counter" [Param "var" "n" "Int"] [FunDecl "inc" [] "" [ForStmt "i" (Bin Range (IntLit 1) (IntLit 5)) [AssignStmt "n" "+=" (Var "i")]]]]

example "package_import" : "package demo.app\nimport kotlin.math.*\nfun id(x: Int): Int = x" parses-to KtFile ["demo", "app"] [ImportDecl ["kotlin", "math"] "*", FunDecl "id" [Param "" "x" "Int"] "Int" [ReturnStmt [Var "x"]]]

example "if_while_method" : "fun loop(n: Int): Int { var s = 0; if (n > 0) { while (s < n) { s += 1 } } else { s = -1 }; return s.dec() }" parses-to KtFile [] [FunDecl "loop" [Param "" "n" "Int"] "Int" [ValStmt True "s" "" [IntLit 0], IfStmt (Bin Gt (Var "n") (IntLit 0)) [WhileStmt (Bin Lt (Var "s") (Var "n")) [AssignStmt "s" "+=" (IntLit 1)]] [AssignStmt "s" "=" (Unary Neg (IntLit 1))], ReturnStmt [MethodCall (Var "s") "dec" []]]]

example "err_expr" : "fun bad(): Int = 1 +" fails-at (1, 21) expecting "expression"

example "err_rbrace" : "fun bad() { val x = 1" fails-at (1, 22) expecting "}"
)PARSLEY"
        },
        StdModule{
            .name = "Haskell",
            .description = "Haskell 2010 declarations, ADTs, equations, and expressions derived from ANTLR4 Haskell",
            .exported_rules = {"haskellModule"},
            .exported_types = {"HsOp", "HsType", "HsCtor", "HsAlt", "HsBind", "HsExpr", "HsDecl", "HsImport", "HsModule"},
            .source = R"PARSLEY(module Haskell (haskellModule) where

import Parsley.Prelude (sepBy, sepEndBy, many, some, optional, eof)

-- Lexical specification (derived from ANTLR4 HaskellLexer.g4)
skip  ws           = /[[:space:]]+/
skip  lineComment  = /--.*/
skip  blockComment = /\{-[^-]*-\}/

token floatLit  = /[0-9]+[.][0-9]+([eE][+-]?[0-9]+)?/ :: Double
token intLit    = /[0-9]+/                              :: Double
token stringLit = /"([^"\\]|\\.)*"/                     :: Text
token charLit   = /'([^'\\]|\\.)'/                      :: Text
token varId     = /[a-z_][a-zA-Z0-9_']*/                :: Text
token conId     = /[A-Z][a-zA-Z0-9_']*/                 :: Text
token modId     = /[A-Z][a-zA-Z0-9_']*([.][A-Z][a-zA-Z0-9_']*)*/ :: Text

keywords =
  [ "module", "where", "import", "qualified", "as", "hiding"
  , "data", "type", "if", "then", "else", "let", "in", "case", "of"
  ]

-- AST (exported to ISO C23 tagged unions)
data HsOp
  = Add | Sub | Mul | Div | Eq | Neq | Lt | Le | Gt | Ge
  | And | Or | Cons | Append | Apply | Compose

data HsType
  = TyCon Text
  | TyVar Text
  | TyApp HsType HsType
  | TyFun HsType HsType
  | TyList HsType
  | TyUnit

data HsCtor
  = HsCtor Text [HsType]

data HsAlt
  = HsAlt Text [Text] HsExpr

data HsBind
  = HsBind Text HsExpr

data HsExpr
  = HsVar Text
  | HsCon Text
  | HsNum Double
  | HsStr Text
  | HsChar Text
  | HsList [HsExpr]
  | HsApp HsExpr [HsExpr]
  | HsBin HsOp HsExpr HsExpr
  | HsNeg HsExpr
  | HsIf HsExpr HsExpr HsExpr
  | HsLet [HsBind] HsExpr
  | HsCase HsExpr [HsAlt]

data HsDecl
  = HsData Text [Text] [HsCtor]
  | HsTypeSyn Text [Text] HsType
  | HsTypeSig Text HsType
  | HsFunBind Text [Text] HsExpr

data HsImport
  = HsImport Bool Text (Maybe Text) [Text]

data HsModule
  = HsModule Text [HsImport] [HsDecl]

-- Grammar rules
haskellModule :: HsModule
haskellModule =
  (do
    modName <- moduleHeader
    imps    <- many (importDecl <* ";")
    decls   <- topDecl `sepEndBy` ";"
    eof
    pure (HsModule modName imps decls))
  <|>
  (do
    imps    <- many (importDecl <* ";")
    decls   <- topDecl `sepEndBy` ";"
    eof
    pure (HsModule "Main" imps decls))

moduleHeader :: Text
moduleHeader = do
  "module"
  name <- modId <|> conId
  optional ("(" *> (exportItem `sepEndBy` ",") <* ")")
  "where"
  optional ";"
  pure name

exportItem :: Text
exportItem = (conId <* optional ("(" *> ".." <* ")")) <|> varId

importDecl :: HsImport
importDecl = do
  "import"
  qual  <- ("qualified" *> pure True) <|> pure False
  mod   <- modId <|> conId
  alias <- (Just <$> ("as" *> (modId <|> conId))) <|> pure Nothing
  optional "hiding"
  items <- ("(" *> (exportItem `sepEndBy` ",") <* ")") <|> pure []
  pure (HsImport qual mod alias items)

topDecl :: HsDecl
topDecl
  =  dataDecl
  |  typeSynDecl
  |  typeSigDecl
  |  funBindDecl

dataDecl :: HsDecl
dataDecl = do
  "data"
  name   <- conId
  params <- many varId
  "="
  ctors  <- hsCtor `sepBy` "|"
  pure (HsData name params ctors)

hsCtor :: HsCtor
hsCtor = HsCtor <$> conId <*> many hsTypeAtom

typeSynDecl :: HsDecl
typeSynDecl = do
  "type"
  name   <- conId
  params <- many varId
  "="
  ty     <- hsType
  pure (HsTypeSyn name params ty)

typeSigDecl :: HsDecl
typeSigDecl = do
  name <- varId
  "::"
  ty   <- hsType
  pure (HsTypeSig name ty)

funBindDecl :: HsDecl
funBindDecl = do
  name   <- varId
  params <- many varId
  "="
  body   <- hsExpr
  pure (HsFunBind name params body)

hsType :: HsType
hsType = operators hsTypeApp
  [ infixr 1 ["->" => TyFun]
  ]

hsTypeApp :: HsType
hsTypeApp
  =  TyApp <$> hsTypeAtom <*> hsTypeAtom
  |  (do
       t <- hsTypeAtom
       pure t)

hsTypeAtom :: HsType
hsTypeAtom
  =  ("[" *> (TyList <$> hsType) <* "]")
  |  ("(" *> ")" *> pure TyUnit)
  |  parensType
  |  TyCon <$> conId
  |  TyVar <$> varId

parensType :: HsType
parensType = "(" *> hsType <* ")"

hsExpr :: HsExpr
hsExpr = operators appExpr
  [ infixr 0 ["$" => HsBin Apply]
  , infixr 2 ["||" => HsBin Or]
  , infixr 3 ["&&" => HsBin And]
  , infix  4 ["==" => HsBin Eq, "/=" => HsBin Neq, "<=" => HsBin Le, ">=" => HsBin Ge, "<" => HsBin Lt, ">" => HsBin Gt]
  , infixr 5 [":" => HsBin Cons, "++" => HsBin Append]
  , infixl 6 ["+" => HsBin Add, "-" => HsBin Sub]
  , infixl 7 ["*" => HsBin Mul, "/" => HsBin Div]
  , infixr 9 ["." => HsBin Compose]
  , prefix 8 ["-" => HsNeg]
  ]

appExpr :: HsExpr
appExpr
  =  HsApp <$> atomExpr <*> some atomExpr
  |  (do
       a <- atomExpr
       pure a)

atomExpr :: HsExpr
atomExpr
  =  ifExpr
  |  letExpr
  |  caseExpr
  |  listExpr
  |  ("(" *> hsExpr <* ")")
  |  HsNum  <$> (floatLit <|> intLit)
  |  HsStr  <$> stringLit
  |  HsChar <$> charLit
  |  HsVar  <$> varId
  |  HsCon  <$> conId

ifExpr :: HsExpr
ifExpr = do
  "if"
  cond <- hsExpr
  "then"
  tBr  <- hsExpr
  "else"
  fBr  <- hsExpr
  pure (HsIf cond tBr fBr)

letBind :: HsBind
letBind = do
  name <- varId
  "="
  val  <- hsExpr
  pure (HsBind name val)

letExpr :: HsExpr
letExpr = do
  "let"
  binds <- ("{" *> (letBind `sepEndBy` ";") <* "}") <|> (letBind `sepEndBy` ";")
  "in"
  body  <- hsExpr
  pure (HsLet binds body)

caseAlt :: HsAlt
caseAlt = do
  pat  <- conId <|> varId
  vars <- many varId
  "->"
  rhs  <- hsExpr
  pure (HsAlt pat vars rhs)

caseSingle :: [HsAlt]
caseSingle = do
  a <- caseAlt
  pure [a]

caseExpr :: HsExpr
caseExpr = do
  "case"
  scrut <- hsExpr
  "of"
  alts  <- ("{" *> (caseAlt `sepEndBy` ";") <* "}") <|> caseSingle
  pure (HsCase scrut alts)

listExpr :: HsExpr
listExpr = HsList <$> ("[" *> (hsExpr `sepBy` ",") <* "]")

-- Conformance examples
example "simple-func" : "module Main where add :: Int -> Int -> Int; add x y = x + y" parses-to HsModule "Main" [] [HsTypeSig "add" (TyFun (TyCon "Int") (TyFun (TyCon "Int") (TyCon "Int"))), HsFunBind "add" ["x", "y"] (HsBin Add (HsVar "x") (HsVar "y"))]
example "data-and-case" : "data Tree a = Leaf a | Node Tree Tree; size t = case t of { Leaf x -> 1; Node l r -> size l + size r }" parses-to HsModule "Main" [] [HsData "Tree" ["a"] [HsCtor "Leaf" [TyVar "a"], HsCtor "Node" [TyCon "Tree", TyCon "Tree"]], HsFunBind "size" ["t"] (HsCase (HsVar "t") [HsAlt "Leaf" ["x"] (HsNum 1), HsAlt "Node" ["l", "r"] (HsBin Add (HsApp (HsVar "size") [HsVar "l"]) (HsApp (HsVar "size") [HsVar "r"]))])]
example "import-and-if" : "import qualified Data.Map as M; clamp x = if x < 0 then 0 else x" parses-to HsModule "Main" [HsImport True "Data.Map" (Just "M") []] [HsFunBind "clamp" ["x"] (HsIf (HsBin Lt (HsVar "x") (HsNum 0)) (HsNum 0) (HsVar "x"))]
example "let-expr" : "calc x = let y = x * 2 in y + 1" parses-to HsModule "Main" [] [HsFunBind "calc" ["x"] (HsLet [HsBind "y" (HsBin Mul (HsVar "x") (HsNum 2))] (HsBin Add (HsVar "y") (HsNum 1)))]
example "list-and-app" : "xs = [1, 2, 3]; main = map f xs" parses-to HsModule "Main" [] [HsFunBind "xs" [] (HsList [HsNum 1, HsNum 2, HsNum 3]), HsFunBind "main" [] (HsApp (HsVar "map") [HsVar "f", HsVar "xs"])]
example "syntax-error" : "f x =" fails-at (1, 6) expecting "expression"
)PARSLEY"
        },
    };
    return modules;
}

std::optional<StdModule> find_builtin_module(std::string_view name) {
    for (const auto& mod : builtin_modules()) {
        if (mod.name == name) {
            return mod;
        }
    }
    return std::nullopt;
}

} // namespace parsley::stdlib
