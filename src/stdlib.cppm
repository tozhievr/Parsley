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
            .description = "Unicode character categories, whitespace, and letter definitions",
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
        }
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
