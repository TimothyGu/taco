#include <iostream>
#include <fstream>
#include <dlfcn.h>
#include <algorithm>
#include <unordered_set>

#include "ir/ir_visitor.h"
#include "codegen_c.h"
#include "taco/util/strings.h"

using namespace std;

namespace taco {
namespace ir {

// Some helper functions
namespace {

// Include stdio.h for printf
// stdlib.h for malloc/realloc
// math.h for sqrt
// MIN preprocessor macro
const string cHeaders = "#ifndef TACO_C_HEADERS\n"
                 "#define TACO_C_HEADERS\n"
                 "#include <stdio.h>\n"
                 "#include <stdlib.h>\n"
                 "#include <math.h>\n"
                 "#define TACO_MIN(_a,_b) ((_a) < (_b) ? (_a) : (_b))\n"
                 "#endif\n";

// find variables for generating declarations
// also only generates a single var for each GetProperty
class FindVars : public IRVisitor {
public:
  map<Expr, string, ExprCompare> varMap;
  
  // this maps from tensor, property, dim to the unique var
  map<tuple<Expr, TensorProperty, int>, string> canonicalPropertyVar;
  
  // this is for convenience, recording just the properties unpacked
  // from the output tensor so we can re-save them at the end
  map<tuple<Expr, TensorProperty, int>, string> outputProperties;
  
  // TODO: should replace this with an unordered set
  vector<Expr> outputTensors;
  
  // copy inputs and outputs into the map
  FindVars(vector<Expr> inputs, vector<Expr> outputs)  {
    for (auto v: inputs) {
      auto var = v.as<Var>();
      iassert(var) << "Inputs must be vars in codegen";
      iassert(varMap.count(var) == 0) << "Duplicate input found in codegen";
      varMap[var] = var->name;
    }
    for (auto v: outputs) {
      auto var = v.as<Var>();
      iassert(var) << "Outputs must be vars in codegen";
      iassert(varMap.count(var) == 0) << "Duplicate output found in codegen";

      outputTensors.push_back(v);
      varMap[var] = var->name;
    }
  }

protected:
  using IRVisitor::visit;

  virtual void visit(const Var *op) {
    if (varMap.count(op) == 0) {
      varMap[op] = CodeGen_C::genUniqueName(op->name);
    }
  }
  
  virtual void visit(const GetProperty *op) {
    if (varMap.count(op) == 0) {
      stringstream name;
      auto tensor = op->tensor.as<Var>();
      //name << "__" << tensor->name << "_";
      name << tensor->name;
      if (op->property == TensorProperty::Values) {
        name << "_vals";
    } else {
      name << "_L" << op->dim;
      if (op->property == TensorProperty::Index)
        name << "_idx";
      if (op->property == TensorProperty::Pointer)
        name << "_ptr";
    }
    auto key = tuple<Expr, TensorProperty, int>
                    (op->tensor, op->property, op->dim);
    if (canonicalPropertyVar.count(key) > 0) {
      varMap[op] = canonicalPropertyVar[key];
    } else {
      auto unique_name = CodeGen_C::genUniqueName(name.str());
      canonicalPropertyVar[key] = unique_name;
      varMap[op] = unique_name;
      if (find(outputTensors.begin(), outputTensors.end(), op->tensor)
          != outputTensors.end()) {
        outputProperties[key] = unique_name;
      }
    }
  }
 }
};



// helper to translate from taco type to C type
string toCType(ComponentType typ, bool is_ptr) {
  string ret;
  
  if (typ == typeOf<int>())
    ret = "int"; //TODO: should use a specific width here
  else if (typ == typeOf<float>())
    ret = "float";
  else if (typ == typeOf<double>())
    ret = "double";
  else
    iassert(false) << "Unknown type in codegen";
  
  if (is_ptr)
    ret += "*";
  
  return ret;
}

// helper to count # of slots for a format
int formatSlots(Format format) {
  int i = 0;
  for (auto level : format.getLevels()) {
    if (level.getType() == LevelType::Dense)
      i += 1;
    else
      i += 2;
  }
  i += 1; // for the vals
  return i;
}

// generate the unpack of a specific property
string unpackTensorProperty(string varname, const GetProperty* op,
                            bool is_output_prop) {
  stringstream ret;
  ret << "  ";
  
  auto tensor = op->tensor.as<Var>();
  if (op->property == TensorProperty::Values) {
    // for the values, it's in the last slot
    ret << toCType(tensor->type, true);
    ret << " restrict " << varname << " = ";
    ret << tensor->name << "[" << formatSlots(tensor->format)-1 << "];\n";
    return ret.str();
  }
  auto levels = tensor->format.getLevels();
  
  iassert(op->dim < levels.size())
    << "Trying to access a nonexistent dimension";
  
  int slot = 0;
  string tp;
  
  for (size_t i=0; i < op->dim; i++) {
    if (levels[i].getType() == LevelType::Dense)
      slot += 1;
    else
      slot += 2;
  }
  
  // for this level, if the property is index, we add 1
  if (op->property == TensorProperty::Index)
    slot += 1;
  
  // for a Dense level, nnz is an int
  // for a Fixed level, ptr is an int
  // all others are int*
  if ((levels[op->dim].getType() == LevelType::Dense &&
      op->property == TensorProperty::Pointer)
      ||(levels[op->dim].getType() == LevelType::Fixed &&
      op->property == TensorProperty::Pointer)) {
    tp = "int";
    ret << tp << " " << varname << " = *(" << tp << "*)" <<
      tensor->name << "[" << slot << "];\n";
  } else {
    tp = "int*";
    ret << tp << " restrict " << varname << " = ";
    ret << "(" << tp << ")" <<
      tensor->name << "[" << slot << "];\n";
  }
  
  return ret.str();
}

string pack_tensor_property(string varname, Expr tnsr, TensorProperty property,
  int dim) {
  stringstream ret;
  ret << "  ";
  
  auto tensor = tnsr.as<Var>();
  if (property == TensorProperty::Values) {
    // for the values, it's in the last slot
    ret << "((double**)" << tensor->name << ")["
        << formatSlots(tensor->format)-1 << "] ";
    ret << " = " << varname << ";\n";
    return ret.str();
  }
  auto levels = tensor->format.getLevels();
  
  iassert(dim < (int)levels.size())
    << "Trying to access a nonexistent dimension";
  
  int slot = 0;
  string tp;
  
  for (int i=0; i<dim; i++) {
    if (levels[i].getType() == LevelType::Dense)
      slot += 1;
    else
      slot += 2;
  }
  
  // for this level, if the property is index, we add 1
  if (property == TensorProperty::Index)
    slot += 1;
  
  // for a Dense level, nnz is an int
  // for a Fixed level, ptr is an int
  // all others are int*
  if ((levels[dim].getType() == LevelType::Dense &&
      property == TensorProperty::Pointer)
      ||(levels[dim].getType() == LevelType::Fixed &&
      property == TensorProperty::Pointer)) {
    tp = "int";
    ret << "*(" << tp << "*)" <<
      tensor->name << "[" << slot << "] = " <<
      varname << ";\n";
  } else {
    tp = "int*";
    ret << "((int**)" << tensor->name
        << ")[" << slot << "] = (" << tp << ")"<< varname
      << ";\n";
  }
  
  return ret.str();
}


// helper to print declarations
string printDecls(map<Expr, string, ExprCompare> varMap,
                   map<tuple<Expr, TensorProperty, int>, string> uniqueProps,
                   vector<Expr> inputs, vector<Expr> outputs) {
  stringstream ret;
  unordered_set<string> propsAlreadyGenerated;
  
  for (auto varpair: varMap) {
    // make sure it's not an input or output
    if (find(inputs.begin(), inputs.end(), varpair.first) == inputs.end() &&
        find(outputs.begin(), outputs.end(), varpair.first) == outputs.end()) {
      auto var = varpair.first.as<Var>();
      if (var) {
        ret << "  " << toCType(var->type, var->is_ptr);
        ret << " " << varpair.second << ";\n";
      } else {
        auto prop = varpair.first.as<GetProperty>();
        iassert(prop);
        if (!propsAlreadyGenerated.count(varpair.second)) {
          // there is an extra deref for output properties, since
          // they are passed by reference
          bool isOutputProp = (find(outputs.begin(), outputs.end(),
                                    prop->tensor) != outputs.end());
          ret << unpackTensorProperty(varpair.second, prop, isOutputProp);
          propsAlreadyGenerated.insert(varpair.second);
        }
      }
    }
  }

  return ret.str();
}



// helper to unpack inputs and outputs
// inputs are unpacked to a pointer
// outputs are unpacked to a pointer
// TODO: this will change for tensors
string printUnpack(vector<Expr> inputs, vector<Expr> outputs) {
  stringstream ret;
  int slot = 0;
  
  for (auto output: outputs) {
    auto var = output.as<Var>();
    if (!var->is_tensor) {

      iassert(var->is_ptr) << "Function outputs must be pointers";

      auto tp = toCType(var->type, var->is_ptr);
      ret << "  " << tp << " " << var->name << " = (" << tp << ")inputPack["
        << slot++ << "];\n";
    } else {
      ret << "  void** " << var->name << " = &(inputPack[" << slot << "]);\n";
      slot += formatSlots(var->format);
    }
  }

  
  for (auto input: inputs) {
    auto var = input.as<Var>();
    if (!var->is_tensor) {
      auto tp = toCType(var->type, var->is_ptr);
      // if the input is not of non-pointer type, we should unpack it
      // here
      auto deref = var->is_ptr ? "" : "*";
      ret << "  " << tp << " " << var->name;
      ret << " = " << deref << "(" << tp << deref << ")inputPack["
        << slot++ << "];\n";
    } else {
      ret << "  void** " << var->name << " = &(inputPack[" << slot << "]);\n";
      slot += formatSlots(var->format);
    }
    
  }
  
  return ret.str();
}

string printPack(map<tuple<Expr, TensorProperty, int>,
                 string> outputProperties) {
  stringstream ret;
  for (auto prop: outputProperties) {
    ret << pack_tensor_property(prop.second, get<0>(prop.first),
      get<1>(prop.first), get<2>(prop.first));
  }
  return ret.str();
}

// seed the unique names with all C99 keywords
// from: http://en.cppreference.com/w/c/keyword
map<string, int> uniqueNameCounters;

void resetUniqueNameCounters() {
  uniqueNameCounters =
    {{"auto", 0},
     {"break", 0},
     {"case", 0},
     {"char", 0},
     {"const", 0},
     {"continue", 0},
     {"default", 0},
     {"do", 0},
     {"double", 0},
     {"else", 0},
     {"enum", 0},
     {"extern", 0},
     {"float", 0},
     {"for", 0},
     {"goto", 0},
     {"if", 0},
     {"inline", 0},
     {"int", 0},
     {"long", 0},
     {"register", 0},
     {"restrict", 0},
     {"return", 0},
     {"short", 0},
     {"signed", 0},
     {"sizeof", 0},
     {"static", 0},
     {"struct", 0},
     {"switch", 0},
     {"typedef", 0},
     {"union", 0},
     {"unsigned", 0},
     {"void", 0},
     {"volatile", 0},
     {"while", 0},
     {"bool", 0},
     {"complex", 0},
     {"imaginary", 0}};
}

} // anonymous namespace


string CodeGen_C::genUniqueName(string name) {
  stringstream os;
  os << name;
  if (uniqueNameCounters.count(name) > 0) {
    os << uniqueNameCounters[name]++;
  } else {
    uniqueNameCounters[name] = 0;
  }
  return os.str();
}

CodeGen_C::CodeGen_C(std::ostream &dest,
                     OutputKind outputKind) : IRPrinter(dest),
  funcBlock(true), out(dest), outputKind(outputKind) {  }
CodeGen_C::~CodeGen_C() { }


void CodeGen_C::compile(Stmt stmt, bool isFirst) {
  if (isFirst && outputKind == C99Implementation) {
    // output the headers
    out << cHeaders;
  }
  // generate code for the Stmt
  stmt.accept(this);
}

void CodeGen_C::visit(const Function* func) {

  // find all the vars that are not inputs or outputs and declare them
  resetUniqueNameCounters();
  FindVars varFinder(func->inputs, func->outputs);
  func->body.accept(&varFinder);
  varMap = varFinder.varMap;
  
  funcDecls = printDecls(varMap, varFinder.canonicalPropertyVar,
    func->inputs, func->outputs);

  // if generating a header, protect the function declaration with a guard
  if (outputKind == C99Header) {
    out << "#ifndef TACO_GENERATED_" << func->name << "\n";
    out << "#define TACO_GENERATED_" << func->name << "\n";
  }

  // output function declaration
  out << "int " << func->name << "(void** inputPack) ";
  
  // if we're just generating a header, this is all we need to do
  if (outputKind == C99Header) {
    out << ";\n";
    out << "#endif\n";
    return;
  }

  do_indent();
  out << "{\n";

  // input/output unpack
  out << printUnpack(func->inputs, func->outputs);

  // output body
  func->body.accept(this);
  
  out << "\n";
  // output repack
  out << printPack(varFinder.outputProperties);
  
  out << "  return 0;\n";
  out << "}\n";

  // clear temporary stuff
  funcBlock = true;
  funcDecls = "";
}

// For Vars, we replace their names with the generated name,
// since we match by reference (not name)
void CodeGen_C::visit(const Var* op) {
  iassert(varMap.count(op) > 0) << "Var " << op->name << " not found in varMap";
  out << varMap[op];
}

namespace {
string genVectorizePragma(int width);
string genVectorizePragma(int width) {
  stringstream ret;
  ret << "#pragma clang loop interleave(enable) ";
  if (!width)
    ret << "vectorize(enable)";
  else
    ret << "vectorize_width(" << width << ")";
  
  return ret.str();
}
}
// The next two need to output the correct pragmas depending
// on the loop kind (Serial, Parallel, Vectorized)
//
// Docs for vectorization pragmas:
// http://clang.llvm.org/docs/LanguageExtensions.html#extensions-for-loop-hint-optimizations
void CodeGen_C::visit(const For* op) {
  if (op->kind == LoopKind::Vectorized) {
    do_indent();
    out << genVectorizePragma(op->vec_width);
    out << "\n";
  }
  
  IRPrinter::visit(op);
}

void CodeGen_C::visit(const While* op) {
  // it's not clear from documentation that clang will vectorize
  // while loops
  // however, we'll output the pragmas anyway
  if (op->kind == LoopKind::Vectorized) {
    do_indent();
    out << genVectorizePragma(op->vec_width);
    out << "\n";
  }
  
  IRPrinter::visit(op);
}



void CodeGen_C::visit(const Block* op) {
  bool outputReturn = funcBlock;
  funcBlock = false;
  
  // if we're the first block in the function, we
  // need to print variable declarations
  if (outputReturn) {
    out << funcDecls;
    indent++;
  }
  
  for (auto s: op->contents) {
    s.accept(this);
    out << "\n";
  }
}

void CodeGen_C::visit(const GetProperty* op) {
  iassert(varMap.count(op) > 0) << "Property of "
      << op->tensor << " not found in varMap";

  out << varMap[op];
}

void CodeGen_C::visit(const Min* op) {
  if (op->operands.size() == 1) {
    op->operands[0].accept(this);
    return;
  }
  for (size_t i=0; i<op->operands.size()-1; i++) {
    stream << "TACO_MIN(";
    op->operands[i].accept(this);
    stream << ",";
  }
  op->operands.back().accept(this);
  for (size_t i=0; i<op->operands.size()-1; i++) {
    stream << ")";
  }

}

void CodeGen_C::visit(const Allocate* op) {
  string elementType = toCType(op->var.type(), false);
  
  op->var.accept(this);
  stream << " = (";
  stream << elementType << "*";
  stream << ")";
  if (op->is_realloc) {
    stream << "realloc(";
    op->var.accept(this);
    stream << ", ";
  }
  else {
    stream << "malloc(";
  }
  stream << "sizeof(" << elementType << ")";
  stream << " * ";
  op->num_elements.accept(this);
  stream << ");";
}

void CodeGen_C::visit(const Sqrt* op) {
  tassert(op->type == typeOf<double>())
    << "Codegen doesn't currently support non-double sqrt";
  stream << "sqrt(";
  op->a.accept(this);
  stream << ")";
}


} // namespace ir
} // namespace taco