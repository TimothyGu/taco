#include "test.h"
#include "taco/tensor.h"
#include "taco/index_notation/index_notation.h"
#include "taco/index_notation/distribution.h"
#include "taco/lower/lower.h"
#include "codegen/codegen.h"
#include "codegen/codegen_legion_c.h"

using namespace taco;

TEST(distributed, test) {
  int dim = 10;
  Tensor<int> a("a", {dim}, Format{Dense});
  Tensor<int> b("b", {dim}, Format{Dense});
  Tensor<int> c("c", {dim}, Format{Dense});

  IndexVar i("i"), in("in"), il("il"), il1 ("il1"), il2("il2");
  a(i) = b(i) + c(i);
  auto stmt = a.getAssignment().concretize();
  stmt = stmt.distribute({i}, {in}, {il}, Grid(4));
  stmt = stmt.split(il, il1, il2, 256);


  // Communication modification must go at the end.
  // TODO (rohany): name -- placement
//  stmt = stmt.pushCommUnder(a(i), in).pushCommUnder(b(i), il1);
//  stmt = stmt.pushCommUnder(a(i), il1).pushCommUnder(b(i), il1);
//  stmt = stmt.pushCommUnder(a(i), in).pushCommUnder(b(i), in);
  stmt = stmt.pushCommUnder(a(i), in).pushCommUnder(b(i), in).pushCommUnder(c(i), in);

  auto lowered = lower(stmt, "computeLegion", false, true);
//  std::cout << lowered << std::endl;

  auto codegen = std::make_shared<ir::CodegenLegionC>(std::cout, taco::ir::CodeGen::ImplementationGen);
  codegen->compile(lowered);
}

TEST(distributed, multiDim) {
  int dim = 10;
  Tensor<int> a("a", {dim, dim}, Format{Dense, Dense});
  Tensor<int> b("b", {dim, dim}, Format{Dense, Dense});

  IndexVar i("i"), j("j"), in("in"), jn("jn"), il("il"), jl("jl");
  a(i, j) = b(i, j);
  auto stmt = a.getAssignment().concretize();
  stmt = stmt.distribute({i, j}, {in, jn}, {il, jl}, Grid(4, 4));
  stmt = stmt.pushCommUnder(a(i, j), jn).pushCommUnder(b(i, j), jn);

  auto lowered = lower(stmt, "computeLegion", false, true);
//  std::cout << lowered << std::endl;
  auto codegen = std::make_shared<ir::CodegenLegionC>(std::cout, taco::ir::CodeGen::ImplementationGen);
  codegen->compile(lowered);
}

TEST(distributed, basicComputeOnto) {
  int dim = 10;
//  Tensor<int> a("a", {dim}, Format{Dense});
//  Tensor<int> b("b", {dim}, Format{Dense});
  Tensor<int> a("a", {dim, dim}, Format{Dense, Dense});
  Tensor<int> b("b", {dim, dim}, Format{Dense, Dense});

  IndexVar i("i"), j("j"), in("in"), jn("jn"), il("il"), jl("jl");
//  a(i) = b(i);
  a(i, j) = b(i, j);
  auto stmt = a.getAssignment().concretize();
//  stmt = stmt.distributeOnto({i}, {in}, {il}, a(i));
  stmt = stmt.distributeOnto({i, j}, {in, jn}, {il, jl}, a(i, j));
//  stmt = stmt.pushCommUnder(b(i), in);
  stmt = stmt.pushCommUnder(b(i, j), jn);

  auto lowered = lower(stmt, "computeLegion", false, true);
//  std::cout << lowered << std::endl;
  auto codegen = std::make_shared<ir::CodegenLegionC>(std::cout, taco::ir::CodeGen::ImplementationGen);
  codegen->compile(lowered);
}

TEST(distributed, summaMM) {
  int dim = 10;
  Tensor<int> a("a", {dim, dim}, Format{Dense, Dense});
  Tensor<int> b("b", {dim, dim}, Format{Dense, Dense});
  Tensor<int> c("c", {dim, dim}, Format{Dense, Dense});

  IndexVar i("i"), j("j"), in("in"), jn("jn"), il("il"), jl("jl"), k("k"), ki("ki"), ko("ko");

  a(i, j) = b(i, k) * c(k, j);
  auto stmt = a.getAssignment().concretize();
  stmt = stmt
      .distributeOnto({i, j}, {in, jn}, {il, jl}, a(i, j))
      .split(k, ko, ki, 256)
      .reorder({ko, il, jl})
      .pushCommUnder(b(i, k), ko)
      .pushCommUnder(c(k, j), ko)
      ;

  auto lowered = lower(stmt, "computeLegion", false, true);
//  std::cout << lowered << std::endl;
  auto codegen = std::make_shared<ir::CodegenLegionC>(std::cout, taco::ir::CodeGen::ImplementationGen);
  codegen->compile(lowered);
}

TEST(distributed, cannonMM) {
  int dim = 10;
  Tensor<int> a("a", {dim, dim}, Format{Dense, Dense});
  Tensor<int> b("b", {dim, dim}, Format{Dense, Dense});
  Tensor<int> c("c", {dim, dim}, Format{Dense, Dense});

  IndexVar i("i"), j("j"), in("in"), jn("jn"), il("il"), jl("jl"), k("k"), ki("ki"), ko("ko"), kos("kos");
  a(i, j) = b(i, k) * c(k, j);
  auto stmt = a.getAssignment().concretize();
  stmt = stmt
      .distributeOnto({i, j}, {in, jn}, {il, jl}, a(i, j))
      .divide(k, ko, ki, 2)
      .reorder({ko, il, jl})
      .stagger(ko, {in, jn}, kos)
      .pushCommUnder(b(i, k), kos)
      .pushCommUnder(c(k, j), kos)
      ;

  auto lowered = lower(stmt, "computeLegion", false, true);
//  std::cout << lowered << std::endl;
  auto codegen = std::make_shared<ir::CodegenLegionC>(std::cout, taco::ir::CodeGen::ImplementationGen);
  codegen->compile(lowered);
}

TEST(distributed, staggerNoDist) {
  int dim = 10;
  Tensor<int> a("a", {dim, dim}, Format{Dense, Dense});
  Tensor<int> b("b", {dim, dim}, Format{Dense, Dense});
  Tensor<int> expected("expected", {dim, dim}, Format{Dense, Dense});

  for (int i = 0; i < dim; i++) {
    for (int j = 0; j < dim; j++) {
      b.insert({i, j}, 1);
      expected.insert({i, j}, 1);
    }
  }

  IndexVar i("i"), j("j"), js("js");
  a(i, j) = b(i, j);
  auto stmt = a.getAssignment().concretize();
  stmt = stmt.stagger(j, {i}, js);

  a.compile(stmt);
  a.evaluate();
  ASSERT_TRUE(equals(a, expected));
}

TEST(distributed, reduction) {
  int dim = 10;
  Tensor<int> a("a", {dim}, Format{Dense});
  Tensor<int> b("b", {dim, dim}, Format{Dense, Dense});
  Tensor<int> c("c", {dim}, Format{Dense});

  IndexVar i("i"), j("j"), in("in"), jn("jn"), il("il"), jl("jl");

  a(i) = b(i, j) * c(j);
  auto stmt = a.getAssignment().concretize();
  stmt = stmt
      .distribute({i, j}, {in, jn}, {il, jl}, Grid(2, 2))
      .pushCommUnder(a(i), jn)
      .pushCommUnder(b(i, j), jn)
      .pushCommUnder(c(j), jn)
      ;

  auto lowered = lower(stmt, "computeLegion", false, true);
//  std::cout << lowered << std::endl;
  auto codegen = std::make_shared<ir::CodegenLegionC>(std::cout, taco::ir::CodeGen::ImplementationGen);
  codegen->compile(lowered);
}

TEST(distributed, nesting) {
  int dim = 10;
  Tensor<int> a("a", {dim, dim}, Format{Dense, Dense});
  Tensor<int> b("b", {dim, dim}, Format{Dense, Dense});

  IndexVar i("i"), j("j");
  a(i, j) = b(j, i);
  auto stmt = a.getAssignment().concretize();
  auto lowered = lower(stmt, "computeLegion", false, true);
  std::cout << lowered << std::endl;
}