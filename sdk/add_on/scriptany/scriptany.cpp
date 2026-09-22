#include "scriptany.h"
#include <new>
#include <assert.h>
#include <string.h>
#include <math.h>                         // ORGLIN: `pow`/`fmod` for `**`/`%`
#include "../scriptarray/scriptarray.h"   // ORGLIN: `any a = [1, 2]`

BEGIN_AS_NAMESPACE

// We'll use the generic interface for the factories as we need the engine pointer
static void ScriptAnyFactory_Generic(asIScriptGeneric *gen)
{
	asIScriptEngine *engine = gen->GetEngine();

	*(CScriptAny**)gen->GetAddressOfReturnLocation() = new CScriptAny(engine);
}

static void ScriptAnyFactory2_Generic(asIScriptGeneric *gen)
{
	asIScriptEngine *engine = gen->GetEngine();
	void *ref = (void*)gen->GetArgAddress(0);
	int refType = gen->GetArgTypeId(0);

	*(CScriptAny**)gen->GetAddressOfReturnLocation() = new CScriptAny(ref,refType,engine);
}

static CScriptAny &ScriptAnyAssignment(CScriptAny *other, CScriptAny *self)
{
	return *self = *other;
}

static void ScriptAnyAssignment_Generic(asIScriptGeneric *gen)
{
	CScriptAny *other = (CScriptAny*)gen->GetArgObject(0);
	CScriptAny *self = (CScriptAny*)gen->GetObject();

	*self = *other;

	gen->SetReturnObject(self);
}

static void ScriptAny_Store_Generic(asIScriptGeneric *gen)
{
	void *ref = (void*)gen->GetArgAddress(0);
	int refTypeId = gen->GetArgTypeId(0);
	CScriptAny *self = (CScriptAny*)gen->GetObject();

	self->Store(ref, refTypeId);
}

static void ScriptAny_StoreInt_Generic(asIScriptGeneric *gen)
{
	asINT64 *ref = (asINT64*)gen->GetArgAddress(0);
	CScriptAny *self = (CScriptAny*)gen->GetObject();

	self->Store(*ref);
}

static void ScriptAny_StoreFlt_Generic(asIScriptGeneric *gen)
{
	double *ref = (double*)gen->GetArgAddress(0);
	CScriptAny *self = (CScriptAny*)gen->GetObject();

	self->Store(*ref);
}

static void ScriptAny_Retrieve_Generic(asIScriptGeneric *gen)
{
	void *ref = (void*)gen->GetArgAddress(0);
	int refTypeId = gen->GetArgTypeId(0);
	CScriptAny *self = (CScriptAny*)gen->GetObject();

	*(bool*)gen->GetAddressOfReturnLocation() = self->Retrieve(ref, refTypeId);
}

// ORGLIN: arithmetic on `any`, so an `any` holding a number behaves like a number
// in an expression (`float z = x + 1.0`). The stored value is read out as a double
// and the operation performed on it; a non-numeric operand is a context exception
// rather than a silently wrong result.
static bool ScriptAnyAsDouble(CScriptAny *a, double &out)
{
	if( a->Retrieve(out) ) return true;
	asINT64 i = 0;
	if( a->Retrieve(i) ) { out = (double)i; return true; }
	return false;
}

static void ScriptAny_CastFail(CScriptAny *self)
{
	asIScriptContext *ctx = asGetActiveContext();
	if( ctx )
		ctx->SetException("any does not hold a value compatible with the requested type");
}

// ORGLIN: the VALUE address of an argument, for overloads whose declared form is
// mixed. This addon registers numbers BOTH ways — by value (`opAdd(double)`,
// `opAssign(int64)`) and by reference (`opAdd(const int64&in)`,
// `opAssign(const ?&in)`) — and in this VM the two are addressed differently:
//
//   GetArgAddress()     DEREFERENCES the stack slot, which is right for a `&in`
//                       parameter or a handle, and returns 0 for a by-value one.
//   GetAddressOfArg()   returns the address of the slot itself, which is right for
//                       a by-value parameter, and for a `&in` one yields a stack
//                       ADDRESS rather than the operand.
//
// So the read has to follow the parameter's declared form, not be picked once for
// the whole overload set. Reading a `&in` operand with GetAddressOfArg was the bug:
// an `any` compared with a literal silently compared a pointer into the VM stack
// (always false), and assigning an object through `?&in` stored that pointer as if
// it were the object and crashed.
static void *ScriptAny_ArgValueAddress(asIScriptGeneric *gen, asUINT arg)
{
	void *addr = gen->GetArgAddress(arg);    // `&in` / handle parameter
	if( addr == 0 )
		addr = gen->GetAddressOfArg(arg);    // by-value parameter
	return addr;
}

#define ORGLIN_ANY_ARITH(NAME, OP)                                                     \
static void ScriptAny_##NAME##_Generic(asIScriptGeneric *gen)                          \
{                                                                                      \
	CScriptAny *self = (CScriptAny*)gen->GetObject();                                  \
	double lhs, rhs;                                                                   \
	if( !ScriptAnyAsDouble(self, lhs) ) { ScriptAny_CastFail(self); return; }           \
	if( gen->GetArgTypeId(0) == asTYPEID_INT64 )                                       \
		rhs = (double)*(asINT64*)ScriptAny_ArgValueAddress(gen, 0);                     \
	else if( gen->GetArgTypeId(0) == asTYPEID_DOUBLE )                                 \
		rhs = *(double*)ScriptAny_ArgValueAddress(gen, 0);                              \
	else { rhs = 0; }                                                                  \
	*(double*)gen->GetAddressOfReturnLocation() = lhs OP rhs;                          \
}

ORGLIN_ANY_ARITH(opAdd, +)
ORGLIN_ANY_ARITH(opSub, -)
ORGLIN_ANY_ARITH(opMul, *)
ORGLIN_ANY_ARITH(opDiv, /)

// ORGLIN: an `any` operand — `a + b` where BOTH sides are `any`. Without these the
// natural dynamic-type expression fails with "No matching operator that takes the
// types 'any&' and 'any&'": the overloads above take a number on the right, so only
// `a + 1` worked, not `a + b`. Both sides are read out as numbers, so the result is
// a plain double like the single-operand form.
#define ORGLIN_ANY_ARITH_ANY(NAME, OP)                                                 \
static void ScriptAny_##NAME##Any_Generic(asIScriptGeneric *gen)                       \
{                                                                                      \
	CScriptAny *self = (CScriptAny*)gen->GetObject();                                  \
	CScriptAny *other = (CScriptAny*)gen->GetArgObject(0);                             \
	double a = 0, b = 0;                                                               \
	if( !ScriptAnyAsDouble(self, a) || other == 0 ||                                   \
		!ScriptAnyAsDouble(other, b) )                                                 \
	{ ScriptAny_CastFail(self); return; }                                              \
	*(double*)gen->GetAddressOfReturnLocation() = a OP b;                              \
}

ORGLIN_ANY_ARITH_ANY(opAdd, +)
ORGLIN_ANY_ARITH_ANY(opSub, -)
ORGLIN_ANY_ARITH_ANY(opMul, *)
ORGLIN_ANY_ARITH_ANY(opDiv, /)

// ORGLIN: the REVERSE operators (`opAdd_r` and friends), which is what makes
// `1 + x` work — a number on the LEFT and the `any` on the right.
//
// AngelScript's overloaded-operator protocol is not symmetric: when the left operand
// has no matching operator it retries with `op<Name>_r` on the RIGHT operand, roughly
// `a OP b` tried again as `b OP_r a`. Since `any` is the only type here that can hold
// arbitrary values, `_r` is the side that has to carry the operator — otherwise a
// primitive LHS simply fails with "No conversion from 'any&' to math type available".
// The arithmetic is identical (all numbers are read out as double); only the operand
// ORDER matters for the non-commutative operators, which is why the macro below keeps
// it explicit rather than reusing the forward one.
#define ORGLIN_ANY_ARITH_R(NAME, OP)                                                   \
static void ScriptAny_##NAME##_r_Generic(asIScriptGeneric *gen)                        \
{                                                                                      \
	CScriptAny *self = (CScriptAny*)gen->GetObject();                                  \
	double rhs = 0, lhs = 0;                                                           \
	if( !ScriptAnyAsDouble(self, rhs) ) { ScriptAny_CastFail(self); return; }           \
	if( gen->GetArgTypeId(0) == asTYPEID_INT64 )                                       \
		lhs = (double)*(asINT64*)ScriptAny_ArgValueAddress(gen, 0);                     \
	else if( gen->GetArgTypeId(0) == asTYPEID_DOUBLE )                                 \
		lhs = *(double*)ScriptAny_ArgValueAddress(gen, 0);                              \
	*(double*)gen->GetAddressOfReturnLocation() = lhs OP rhs;                          \
}

ORGLIN_ANY_ARITH_R(opAdd, +)
ORGLIN_ANY_ARITH_R(opSub, -)
ORGLIN_ANY_ARITH_R(opMul, *)
ORGLIN_ANY_ARITH_R(opDiv, /)

// ORGLIN: `%` (modulo), `**` (exponent) and unary `-`. These cannot share the macro
// above: C++ `%` is not valid on a double (so modulo goes through `fmod`), `**` is
// `pow`, and unary minus takes no right-hand operand at all.
static void ScriptAny_opMod_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	double lhs = 0, rhs = 0;
	if( !ScriptAnyAsDouble(self, lhs) ) { ScriptAny_CastFail(self); return; }
	if( gen->GetArgTypeId(0) == asTYPEID_INT64 )
		rhs = (double)*(asINT64*)ScriptAny_ArgValueAddress(gen, 0);
	else if( gen->GetArgTypeId(0) == asTYPEID_DOUBLE )
		rhs = *(double*)ScriptAny_ArgValueAddress(gen, 0);
	*(double*)gen->GetAddressOfReturnLocation() = fmod(lhs, rhs);
}

static void ScriptAny_opPow_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	double lhs = 0, rhs = 0;
	if( !ScriptAnyAsDouble(self, lhs) ) { ScriptAny_CastFail(self); return; }
	if( gen->GetArgTypeId(0) == asTYPEID_INT64 )
		rhs = (double)*(asINT64*)ScriptAny_ArgValueAddress(gen, 0);
	else if( gen->GetArgTypeId(0) == asTYPEID_DOUBLE )
		rhs = *(double*)ScriptAny_ArgValueAddress(gen, 0);
	*(double*)gen->GetAddressOfReturnLocation() = pow(lhs, rhs);
}

static void ScriptAny_opNeg_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	double v = 0;
	if( !ScriptAnyAsDouble(self, v) ) { ScriptAny_CastFail(self); return; }
	*(double*)gen->GetAddressOfReturnLocation() = -v;
}

// The reverse forms of `%` and `**` (see ORGLIN_ANY_ARITH_R) — a number on the left.
static void ScriptAny_opPow_r_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	double rhs = 0, lhs = 0;
	if( !ScriptAnyAsDouble(self, rhs) ) { ScriptAny_CastFail(self); return; }
	if( gen->GetArgTypeId(0) == asTYPEID_INT64 )
		lhs = (double)*(asINT64*)ScriptAny_ArgValueAddress(gen, 0);
	else if( gen->GetArgTypeId(0) == asTYPEID_DOUBLE )
		lhs = *(double*)ScriptAny_ArgValueAddress(gen, 0);
	*(double*)gen->GetAddressOfReturnLocation() = pow(lhs, rhs);
}

static void ScriptAny_opMod_r_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	double rhs = 0, lhs = 0;
	if( !ScriptAnyAsDouble(self, rhs) ) { ScriptAny_CastFail(self); return; }
	if( gen->GetArgTypeId(0) == asTYPEID_INT64 )
		lhs = (double)*(asINT64*)ScriptAny_ArgValueAddress(gen, 0);
	else if( gen->GetArgTypeId(0) == asTYPEID_DOUBLE )
		lhs = *(double*)ScriptAny_ArgValueAddress(gen, 0);
	*(double*)gen->GetAddressOfReturnLocation() = fmod(lhs, rhs);
}

// ORGLIN: COMPOUND ASSIGNMENT (`x += 2`). AngelScript does NOT desugar this into
// `x = x + 2` for an object type, so the operator has to exist explicitly or the
// whole family fails with "Illegal operation on 'any&'". The result is stored back
// into the `any`, which is what makes `any x = 1; x += 2` leave 3 behind — the
// `opAdd` above only produces a value.
#define ORGLIN_ANY_OPASSIGN(NAME, EXPR)                                                \
static void ScriptAny_##NAME##_Generic(asIScriptGeneric *gen)                          \
{                                                                                      \
	CScriptAny *self = (CScriptAny*)gen->GetObject();                                  \
	double lhs = 0, rhs = 0;                                                           \
	int argType = gen->GetArgTypeId(0);                                                \
	if( !ScriptAnyAsDouble(self, lhs) ) { ScriptAny_CastFail(self); return; }           \
	if( argType == asTYPEID_INT64 )                                                    \
		rhs = (double)*(asINT64*)ScriptAny_ArgValueAddress(gen, 0);                     \
	else if( argType == asTYPEID_DOUBLE )                                              \
		rhs = *(double*)ScriptAny_ArgValueAddress(gen, 0);                              \
	double next = (EXPR);                                                              \
	self->Store(next);                                                                 \
	gen->SetReturnObject(self);                                                       \
}

ORGLIN_ANY_OPASSIGN(opAddAssign, lhs + rhs)
ORGLIN_ANY_OPASSIGN(opSubAssign, lhs - rhs)
ORGLIN_ANY_OPASSIGN(opMulAssign, lhs * rhs)
ORGLIN_ANY_OPASSIGN(opDivAssign, lhs / rhs)
ORGLIN_ANY_OPASSIGN(opModAssign, fmod(lhs, rhs))

// ORGLIN: `x++` / `--x` on a numeric `any`. Reading the held value, stepping it by
// one, and storing it back keeps `any` behaving like the number it holds. A
// non-numeric `any` raises the same exception the arithmetic does, so `x++` on a
// string is loud rather than a silent no-op.
//
// The dispatch is on the STORED type id, NOT on "which Retrieve succeeds": the int64
// and double overloads of Retrieve are inter-convertible, so asking for int64 first
// silently TRUNCATES a held double (`any d = 1.5; ++d` used to leave 2, not 2.5).
// The stepped value also goes into a NAMED local, because every `Store` overload
// takes a non-const reference and so cannot bind to a temporary.
#define ORGLIN_ANY_STEP(NAME, OP)                                                      \
static void ScriptAny_##NAME##_Generic(asIScriptGeneric *gen)                          \
{                                                                                      \
	CScriptAny *self = (CScriptAny*)gen->GetObject();                                  \
	int typeId = self->GetTypeId();                                                    \
	if( typeId == asTYPEID_INT64 )                                                     \
	{                                                                                  \
		asINT64 v = 0;                                                                 \
		if( !self->Retrieve(v) ) { ScriptAny_CastFail(self); return; }                  \
		asINT64 next = v OP 1;                                                        \
		self->Store(next);                                                             \
	}                                                                                  \
	else if( typeId == asTYPEID_DOUBLE )                                               \
	{                                                                                  \
		double v = 0;                                                                  \
		if( !self->Retrieve(v) ) { ScriptAny_CastFail(self); return; }                  \
		double next = v OP 1.0;                                                       \
		self->Store(next);                                                             \
	}                                                                                  \
	else { ScriptAny_CastFail(self); return; }                                         \
	gen->SetReturnObject(self);                                                       \
}

ORGLIN_ANY_STEP(opPreInc,  +)
ORGLIN_ANY_STEP(opPostInc, +)
ORGLIN_ANY_STEP(opPreDec,  -)
ORGLIN_ANY_STEP(opPostDec, -)

// ORGLIN: numeric extraction helpers for the value-returning conversions.
static void ScriptAny_opConvInt_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	asINT64 v = 0;
	double d = 0;
	if     ( self->Retrieve(v) ) { /* already an integer */ }
	else if( self->Retrieve(d) ) { v = (asINT64)d; }
	*(asINT64*)gen->GetAddressOfReturnLocation() = v;
}

static void ScriptAny_opConvDouble_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	double d = 0;
	asINT64 v = 0;
	if     ( self->Retrieve(d) ) { /* already a double */ }
	else if( self->Retrieve(v) ) { d = (double)v; }
	*(double*)gen->GetAddressOfReturnLocation() = d;
}

// ORGLIN: direct value assignment, so `x = 2.5` / `x = 7` store instead of failing
// (these mirror what the dictionary addon registers on `dictionaryValue`).
static void ScriptAny_opAssignDbl_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	double v = *(double*)ScriptAny_ArgValueAddress(gen, 0);
	self->Store(v);
	gen->SetReturnObject(self);
}

static void ScriptAny_opAssignInt_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	asINT64 v = *(asINT64*)ScriptAny_ArgValueAddress(gen, 0);
	self->Store(v);
	gen->SetReturnObject(self);
}

// ORGLIN: generic `?&in` assignment — the catch-all that lets `any x = <anything>`
// work once the value-taking constructor is gone. The argument's runtime type id is
// what the addon's own `store(?&in)` uses, so this is the same path.
static void ScriptAny_opAssignAny_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	void *ref = ScriptAny_ArgValueAddress(gen, 0);
	int typeId = gen->GetArgTypeId(0);
	self->Store(ref, typeId);
	gen->SetReturnObject(self);
}

// ORGLIN: `any` compared with another `any`, or with a plain number. Both operands
// are read as numbers; comparing an `any` holding a string against a number is
// false rather than an error, which is what "==" means for a dynamic type.
static bool ScriptAny_ArgAsDouble(asIScriptGeneric *gen, int argIndex, double &out)
{
	if( gen->GetArgTypeId(argIndex) == asTYPEID_INT64 )
		out = (double)*(asINT64*)ScriptAny_ArgValueAddress(gen, argIndex);
	else if( gen->GetArgTypeId(argIndex) == asTYPEID_DOUBLE )
		out = *(double*)ScriptAny_ArgValueAddress(gen, argIndex);
	else
		return false;
	return true;
}

static void ScriptAny_opEqualsAny_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	CScriptAny *other = (CScriptAny*)gen->GetArgObject(0);
	double a = 0, b = 0;
	bool okA = ScriptAnyAsDouble(self, a);
	bool okB = other != 0 && ScriptAnyAsDouble(other, b);
	*(bool*)gen->GetAddressOfReturnLocation() = (okA && okB && a == b);
}

static void ScriptAny_opEqualsNum_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	double a = 0, b = 0;
	bool okB = ScriptAny_ArgAsDouble(gen, 0, b);
	bool okA = ScriptAnyAsDouble(self, a);
	*(bool*)gen->GetAddressOfReturnLocation() = (okA && okB && a == b);
}

static void ScriptAny_opNotEqualsAny_Generic(asIScriptGeneric *gen)
{
	ScriptAny_opEqualsAny_Generic(gen);
	bool *r = (bool*)gen->GetAddressOfReturnLocation();
	*r = !*r;
}

static void ScriptAny_opNotEqualsNum_Generic(asIScriptGeneric *gen)
{
	ScriptAny_opEqualsNum_Generic(gen);
	bool *r = (bool*)gen->GetAddressOfReturnLocation();
	*r = !*r;
}

// ORGLIN: implicit extraction (`opCast(?&out)`), so an `any` can be used where the
// stored type is expected — e.g. `float z = x` reads the value out of `x`.
// A failed extraction is a real error, not a silent zero: the whole point of a
// dynamic type is that the wrong TYPE is reported, not swallowed.
static void ScriptAny_opCast_Generic(asIScriptGeneric *gen)
{
	void *ref = (void*)gen->GetArgAddress(0);
	int refTypeId = gen->GetArgTypeId(0);
	CScriptAny *self = (CScriptAny*)gen->GetObject();

	if( !self->Retrieve(ref, refTypeId) )
		ScriptAny_CastFail(self);
}

static void ScriptAny_RetrieveInt_Generic(asIScriptGeneric *gen)
{
	asINT64 *ref = (asINT64*)gen->GetArgAddress(0);
	CScriptAny *self = (CScriptAny*)gen->GetObject();

	*(bool*)gen->GetAddressOfReturnLocation() = self->Retrieve(*ref);
}

static void ScriptAny_RetrieveFlt_Generic(asIScriptGeneric *gen)
{
	double *ref = (double*)gen->GetArgAddress(0);
	CScriptAny *self = (CScriptAny*)gen->GetObject();

	*(bool*)gen->GetAddressOfReturnLocation() = self->Retrieve(*ref);
}

static void ScriptAny_AddRef_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	self->AddRef();
}

static void ScriptAny_Release_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	self->Release();
}

static void ScriptAny_GetRefCount_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	*(int*)gen->GetAddressOfReturnLocation() = self->GetRefCount();
}

static void ScriptAny_SetFlag_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	self->SetFlag();
}

static void ScriptAny_GetFlag_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	*(bool*)gen->GetAddressOfReturnLocation() = self->GetFlag();
}

static void ScriptAny_EnumReferences_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	asIScriptEngine *engine = *(asIScriptEngine**)gen->GetAddressOfArg(0);
	self->EnumReferences(engine);
}

static void ScriptAny_ReleaseAllHandles_Generic(asIScriptGeneric *gen)
{
	CScriptAny *self = (CScriptAny*)gen->GetObject();
	asIScriptEngine *engine = *(asIScriptEngine**)gen->GetAddressOfArg(0);
	self->ReleaseAllHandles(engine);
}

void RegisterScriptAny(asIScriptEngine *engine)
{
	if( strstr(asGetLibraryOptions(), "AS_MAX_PORTABILITY") )
		RegisterScriptAny_Generic(engine);
	else
		RegisterScriptAny_Native(engine);
}

//--------------------------------------------------------------------------
// ORGLIN (ADR-0011): `any` as the destination of an ARRAY LIST LITERAL.
//
//   any a = [1, 2.5]        -> an `array<any>` holding 1 and 2.5
//   any a = [1, "two", 3.5] -> a MIXED `array<any>`, which is the point of `any`
//   any a = []              -> an empty `array<any>`
//
// WHY THIS EXISTS. A list literal is compiled by resolving the DECLARED type's
// list factory (`CompileInitList`), and `any` had none — so `any a = [1, 2]`
// failed with "Initialization lists cannot be used with 'any'" and the only way
// to give an `any` a container was to build the container in a variable first.
// A factory with a `?` element pattern is what was missing, and it is also what
// makes ONE element type for all of the above unnecessary.
//
// `any a = { k = v }` is NOT covered, and cannot be from here: a question slot
// cannot determine an element's type, so the compiler rejects a dictionary
// literal with "Initialization lists cannot be used with '?'" before any factory
// is reached. (Measured, not assumed: the empty `{}` has no elements at all, so
// it lowers to an EMPTY `array<any>`, and both of the paths that would have
// produced a dictionary fail at compile — so there is no dictionary case to
// implement here.) Assign a `dictionary` variable instead.
//
// THE BUFFER. By the time the factory runs, the compiler has already evaluated
// every element and, because the pattern names no type, inlined a type id before
// each value (`asBC_SetListType`) and a count before the whole run
// (`asBC_SetListSize`) — the same shape `CScriptArray` parses in its own list
// factory. So this walks the buffer and, for each element, builds one `any` to
// hold it; the wrapper `any` is then filled with the resulting `array<any>`.
//   - A narrow primitive (an `int` literal arrives as asTYPEID_INT32) is widened
//     first: `CScriptAny::Store` only accepts objects and the three WIDE
//     primitives (bool/int64/double), exactly as the dictionary addon's own
//     buffer reader does for its elements.
//   - The elements are transferred through `array<any>`'s OWN list factory rather
//     than assembled by hand, so their lifetime is the one an explicit
//     `array<any> a = [any(1), any(2)]` already has.
static asBYTE *ScriptAnyListNextValue(asIScriptEngine *engine, asBYTE *buffer, int typeId)
{
	// Align to 4 bytes, exactly as the buffer was packed and as the dictionary
	// addon reads it back.
	if( asPWORD(buffer) & 0x3 )
		buffer += 4 - (asPWORD(buffer) & 0x3);

	if( typeId & asTYPEID_MASK_OBJECT )
	{
		asITypeInfo *ti = engine->GetTypeInfoById(typeId);
		if( ti && (ti->GetFlags() & asOBJ_VALUE) )
			buffer += ti->GetSize();
		else
			buffer += sizeof(void*);
	}
	else if( typeId == 0 )
	{
		buffer += sizeof(void*);   // a null handle
	}
	else
	{
		buffer += engine->GetSizeOfPrimitiveType(typeId);
	}
	return buffer;
}

// Store one list element into a fresh `any`. `CScriptAny::Store(void*, int)` only
// accepts objects and the three WIDE primitives (bool/int64/double), so a narrow
// primitive — an `int` literal arrives as asTYPEID_INT32 — must be widened here
// first, exactly as the dictionary addon does for its own buffer elements. Without
// this the addon's own assertion fires on `any a = [1, 2]`.
static void ScriptAnyStoreElement(CScriptAny *dst, void *value, int typeId)
{
	if( (typeId & asTYPEID_MASK_OBJECT) || typeId == 0 ||
		typeId == asTYPEID_BOOL || typeId == asTYPEID_INT64 || typeId == asTYPEID_DOUBLE )
	{
		dst->Store(value, typeId);
		return;
	}

	if( typeId >= asTYPEID_FLOAT )
	{
		double d = 0;
		switch( typeId )
		{
		case asTYPEID_FLOAT:  d = *(float*)value;  break;
		case asTYPEID_DOUBLE: d = *(double*)value; break;
		}
		dst->Store(d);
	}
	else
	{
		asINT64 i64 = 0;
		switch( typeId )
		{
		case asTYPEID_INT8:   i64 = *(char*)value;           break;
		case asTYPEID_INT16:  i64 = *(short*)value;          break;
		case asTYPEID_INT32:  i64 = *(int*)value;            break;
		case asTYPEID_INT64:  i64 = *(asINT64*)value;        break;
		case asTYPEID_UINT8:  i64 = *(unsigned char*)value;  break;
		case asTYPEID_UINT16: i64 = *(unsigned short*)value; break;
		case asTYPEID_UINT32: i64 = *(unsigned int*)value;   break;
		case asTYPEID_UINT64: i64 = *(asINT64*)value;        break;
		}
		dst->Store(i64);
	}
}

static void ScriptAnyListFactory_Generic(asIScriptGeneric *gen)
{
	asIScriptEngine *engine = gen->GetEngine();
	asBYTE *buffer = (asBYTE*)gen->GetArgAddress(0);
	if( buffer == 0 )
	{
		asIScriptContext *ctx = asGetActiveContext();
		if( ctx )
			ctx->SetException("Internal error: no list buffer for an `any` literal");
		return;
	}

	asUINT count = *(asUINT*)buffer;
	asBYTE *p = buffer + 4;

	// Every element becomes one element of an `array<any>`.
	//
	// Build a temporary buffer in the exact shape array<T>'s OWN list factory
	// reads ([count][handle]...), then let it perform the transfer. Delegating is
	// what keeps the lifetime right: that factory memcpy's the handles out and
	// ZEROES the source, so the elements move into the array precisely as
	// `array<any> a = [any(1), any(2)]` already moves them — no hand-rolled
	// AddRef/Release, and the compiler's later buffer destroy finds nothing left
	// to release. A hand-built array instead leaves stale element pointers in the
	// buffer, which is a hard crash at cleanup.
	asITypeInfo *arrType = engine->GetTypeInfoByDecl("array<any>");
	if( arrType == 0 )
	{
		asIScriptContext *ctx = asGetActiveContext();
		if( ctx )
			ctx->SetException("Internal error: `array<any>` is not registered");
		return;
	}

	const asUINT tmpSize = 4 + count * asUINT(sizeof(void*));
	asBYTE *tmp = (asBYTE*)asAllocMem(tmpSize);
	if( tmp == 0 )
	{
		asIScriptContext *ctx = asGetActiveContext();
		if( ctx )
			ctx->SetException("Out of memory building an `any` array");
		return;
	}
	memset(tmp, 0, tmpSize);
	*(asUINT*)tmp = count;

	CScriptAny **slots = (CScriptAny**)(tmp + 4);
	for( asUINT n = 0; n < count; ++n )
	{
		if( asPWORD(p) & 0x3 )
			p += 4 - (asPWORD(p) & 0x3);

		int typeId = *(int*)p;
		p += sizeof(int);

		// One `any` per element, holding the element's own value. The element is
		// still alive in the buffer at this point: Store() copies primitives, and
		// for objects it either clones a value type or takes its own reference.
		slots[n] = new CScriptAny(engine);
		ScriptAnyStoreElement(slots[n], (void*)p, typeId);

		p = ScriptAnyListNextValue(engine, p, typeId);
	}

	CScriptArray *arr = CScriptArray::Create(arrType, tmp);
	asFreeMem(tmp);

	CScriptAny *result = new CScriptAny(engine);
	result->Store(&arr, asTYPEID_OBJHANDLE | arrType->GetTypeId());
	arr->Release();   // Store() took its own reference

	*(CScriptAny**)gen->GetAddressOfReturnLocation() = result;
}

void RegisterScriptAny_Native(asIScriptEngine *engine)
{
	int r;
	r = engine->RegisterObjectType("any", sizeof(CScriptAny), asOBJ_REF | asOBJ_GC); assert( r >= 0 );

	// We'll use the generic interface for the constructor as we need the engine pointer.
	//
	// ORGLIN: the value-taking constructors are kept, but NOT `explicit` — and they
	// are load-bearing rather than a spelling the author needs. A bare array literal
	// (`array<any> xs = [1, 2.5]`) is built by array<T>'s list factory, which needs a
	// conversion from each element into `T`; that conversion IS this constructor.
	// Removing it makes `array<any> = [1, 2]` fail with "Can't implicitly convert
	// from 'const int' to 'any'", and marking it `explicit` (upstream's form) fails
	// the same way — which is why upstream requires `any(1)` inside the literal.
	// They are non-explicit so the literal works; `any(1)` therefore also compiles,
	// but it is never NEEDED — `any x = 1` goes through opAssign below.
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_FACTORY, "any@ f()", asFUNCTION(ScriptAnyFactory_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_FACTORY, "any@ f(?&in)", asFUNCTION(ScriptAnyFactory2_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_FACTORY, "any@ f(const int64&in)", asFUNCTION(ScriptAnyFactory2_Generic), asCALL_GENERIC); assert(r >= 0);
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_FACTORY, "any@ f(const double&in)", asFUNCTION(ScriptAnyFactory2_Generic), asCALL_GENERIC); assert(r >= 0);

	// ORGLIN (ADR-0011): `any a = [ ... ]` / `any a = { k = v }` — the list factory
	// that makes a container LITERAL assignable to `any`
	// (see ScriptAnyListFactory_Generic). Without it a literal into `any` fails with
	// "Initialization lists cannot be used with 'any'".
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_LIST_FACTORY, "any@f(int &in) {repeat ?}", asFUNCTION(ScriptAnyListFactory_Generic), asCALL_GENERIC); assert( r >= 0 );

	// ORGLIN: conversion OUT of `any` — the `opConv` family (`double(x)`, `int64(x)`).
	r = engine->RegisterObjectMethod("any", "void opCast(?&out)", asFUNCTION(ScriptAny_opCast_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "void opConv(?&out)", asFUNCTION(ScriptAny_opCast_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "int64 opConv()", asFUNCTION(ScriptAny_opConvInt_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opConv()", asFUNCTION(ScriptAny_opConvDouble_Generic), asCALL_GENERIC); assert( r >= 0 );
	// Comparisons, so `any` has the operators every other type has.
	r = engine->RegisterObjectMethod("any", "bool opEquals(const any &in)", asFUNCTION(ScriptAny_opEqualsAny_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "bool opEquals(const int64 &in)", asFUNCTION(ScriptAny_opEqualsNum_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "bool opEquals(double)", asFUNCTION(ScriptAny_opEqualsNum_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "bool opNotEquals(const any &in)", asFUNCTION(ScriptAny_opNotEqualsAny_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "bool opNotEquals(const int64 &in)", asFUNCTION(ScriptAny_opNotEqualsNum_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "bool opNotEquals(double)", asFUNCTION(ScriptAny_opNotEqualsNum_Generic), asCALL_GENERIC); assert( r >= 0 );

	r = engine->RegisterObjectBehaviour("any", asBEHAVE_ADDREF, "void f()", asMETHOD(CScriptAny,AddRef), asCALL_THISCALL); assert( r >= 0 );
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_RELEASE, "void f()", asMETHOD(CScriptAny,Release), asCALL_THISCALL); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opAssign(any&in)", asFUNCTION(ScriptAnyAssignment), asCALL_CDECL_OBJLAST); assert( r >= 0 );
	// ORGLIN: value assignment for every other type, so `any x = 1`, `x = "s"` and
	// assigning inside an `array<any>` literal all work WITHOUT a constructor or a
	// `.store()` call. The `?&in` overload covers objects (dictionary, array, other
	// script types); the int64/double overloads mirror the addon's own store()
	// overloads so all numbers normalise to 64-bit.
	r = engine->RegisterObjectMethod("any", "any &opAssign(const ?&in)", asFUNCTION(ScriptAny_opAssignAny_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opAssign(double)", asFUNCTION(ScriptAny_opAssignDbl_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opAssign(int64)", asFUNCTION(ScriptAny_opAssignInt_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "void store(?&in)", asMETHODPR(CScriptAny,Store,(void*,int),void), asCALL_THISCALL); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "void store(const int64&in)", asMETHODPR(CScriptAny,Store,(asINT64&),void), asCALL_THISCALL); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "void store(const double&in)", asMETHODPR(CScriptAny,Store,(double&),void), asCALL_THISCALL); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "bool retrieve(?&out) const", asMETHODPR(CScriptAny,Retrieve,(void*,int) const,bool), asCALL_THISCALL); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "bool retrieve(int64&out) const", asMETHODPR(CScriptAny,Retrieve,(asINT64&) const,bool), asCALL_THISCALL); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "bool retrieve(double&out) const", asMETHODPR(CScriptAny,Retrieve,(double&) const,bool), asCALL_THISCALL); assert( r >= 0 );

	// ORGLIN: ARITHMETIC on `any`, so a numeric `any` behaves like a number
	// (`any x = 1; double r = x + 1`). A non-numeric operand is a context exception
	// rather than a silently wrong result.
	//
	// These were registered ONLY in RegisterScriptAny_Generic, which this build never
	// calls — MSVC compiles without AS_MAX_PORTABILITY, so RegisterScriptAny takes the
	// NATIVE branch and `x + 1` failed with "No conversion from 'any&' to math type
	// available". The handlers are generic-call functions either way, so they work
	// unchanged from here; that is why the omission was invisible for so long.
	// The `const int64&in` overload is NOT optional: an `int` literal is widened to
	// int64 by overload resolution, so without it `x + 1` matches nothing.
	r = engine->RegisterObjectMethod("any", "double opAdd(double)", asFUNCTION(ScriptAny_opAdd_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opAdd(const int64&in)", asFUNCTION(ScriptAny_opAdd_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opSub(double)", asFUNCTION(ScriptAny_opSub_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opSub(const int64&in)", asFUNCTION(ScriptAny_opSub_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opMul(double)", asFUNCTION(ScriptAny_opMul_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opMul(const int64&in)", asFUNCTION(ScriptAny_opMul_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opDiv(double)", asFUNCTION(ScriptAny_opDiv_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opDiv(const int64&in)", asFUNCTION(ScriptAny_opDiv_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opAdd(const any &in)", asFUNCTION(ScriptAny_opAddAny_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opSub(const any &in)", asFUNCTION(ScriptAny_opSubAny_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opMul(const any &in)", asFUNCTION(ScriptAny_opMulAny_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opDiv(const any &in)", asFUNCTION(ScriptAny_opDivAny_Generic), asCALL_GENERIC); assert( r >= 0 );

	// ORGLIN: the REVERSE operators, so a NUMBER on the left works (`1 + x`). Without
	// them a primitive LHS fails with "No conversion from 'any&' to math type
	// available" — see the handler comment for the protocol.
	r = engine->RegisterObjectMethod("any", "double opAdd_r(double)", asFUNCTION(ScriptAny_opAdd_r_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opAdd_r(const int64&in)", asFUNCTION(ScriptAny_opAdd_r_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opSub_r(double)", asFUNCTION(ScriptAny_opSub_r_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opSub_r(const int64&in)", asFUNCTION(ScriptAny_opSub_r_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opMul_r(double)", asFUNCTION(ScriptAny_opMul_r_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opMul_r(const int64&in)", asFUNCTION(ScriptAny_opMul_r_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opDiv_r(double)", asFUNCTION(ScriptAny_opDiv_r_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opDiv_r(const int64&in)", asFUNCTION(ScriptAny_opDiv_r_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opPow_r(double)", asFUNCTION(ScriptAny_opPow_r_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opPow_r(const int64&in)", asFUNCTION(ScriptAny_opPow_r_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opMod_r(double)", asFUNCTION(ScriptAny_opMod_r_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opMod_r(const int64&in)", asFUNCTION(ScriptAny_opMod_r_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opMod(double)", asFUNCTION(ScriptAny_opMod_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opMod(const int64&in)", asFUNCTION(ScriptAny_opMod_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opPow(double)", asFUNCTION(ScriptAny_opPow_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opPow(const int64&in)", asFUNCTION(ScriptAny_opPow_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opNeg()", asFUNCTION(ScriptAny_opNeg_Generic), asCALL_GENERIC); assert( r >= 0 );

	// ORGLIN: compound assignment (`x += 2`), which an object type does NOT get for
	// free from `x = x + 2` — see the handlers.
	r = engine->RegisterObjectMethod("any", "any &opAddAssign(double)", asFUNCTION(ScriptAny_opAddAssign_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opAddAssign(const int64&in)", asFUNCTION(ScriptAny_opAddAssign_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opSubAssign(double)", asFUNCTION(ScriptAny_opSubAssign_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opSubAssign(const int64&in)", asFUNCTION(ScriptAny_opSubAssign_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opMulAssign(double)", asFUNCTION(ScriptAny_opMulAssign_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opMulAssign(const int64&in)", asFUNCTION(ScriptAny_opMulAssign_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opDivAssign(double)", asFUNCTION(ScriptAny_opDivAssign_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opDivAssign(const int64&in)", asFUNCTION(ScriptAny_opDivAssign_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opModAssign(double)", asFUNCTION(ScriptAny_opModAssign_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opModAssign(const int64&in)", asFUNCTION(ScriptAny_opModAssign_Generic), asCALL_GENERIC); assert( r >= 0 );

	// ORGLIN: INCREMENT / DECREMENT, so `x++` on a numeric `any` advances the value it
	// holds. Same numeric-only rule as the arithmetic above: a non-numeric `any` is an
	// exception rather than a silent no-op.
	r = engine->RegisterObjectMethod("any", "any &opPreInc()", asFUNCTION(ScriptAny_opPreInc_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opPostInc()", asFUNCTION(ScriptAny_opPostInc_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opPreDec()", asFUNCTION(ScriptAny_opPreDec_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opPostDec()", asFUNCTION(ScriptAny_opPostDec_Generic), asCALL_GENERIC); assert( r >= 0 );

	// Register GC behaviours
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_GETREFCOUNT, "int f()", asMETHOD(CScriptAny,GetRefCount), asCALL_THISCALL); assert( r >= 0 );
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_SETGCFLAG, "void f()", asMETHOD(CScriptAny,SetFlag), asCALL_THISCALL); assert( r >= 0 );
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_GETGCFLAG, "bool f()", asMETHOD(CScriptAny,GetFlag), asCALL_THISCALL); assert( r >= 0 );
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_ENUMREFS, "void f(int&in)", asMETHOD(CScriptAny,EnumReferences), asCALL_THISCALL); assert( r >= 0 );
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_RELEASEREFS, "void f(int&in)", asMETHOD(CScriptAny,ReleaseAllHandles), asCALL_THISCALL); assert( r >= 0 );
}

void RegisterScriptAny_Generic(asIScriptEngine *engine)
{
	int r;
	r = engine->RegisterObjectType("any", sizeof(CScriptAny), asOBJ_REF | asOBJ_GC); assert( r >= 0 );

	// We'll use the generic interface for the constructor as we need the engine pointer
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_FACTORY, "any@ f()", asFUNCTION(ScriptAnyFactory_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_FACTORY, "any@ f(?&in)", asFUNCTION(ScriptAnyFactory2_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_FACTORY, "any@ f(const int64&in)", asFUNCTION(ScriptAnyFactory2_Generic), asCALL_GENERIC); assert(r >= 0);
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_FACTORY, "any@ f(const double&in)", asFUNCTION(ScriptAnyFactory2_Generic), asCALL_GENERIC); assert(r >= 0);

	r = engine->RegisterObjectMethod("any", "any &opAssign(double)", asFUNCTION(ScriptAny_opAssignDbl_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opAssign(int64)", asFUNCTION(ScriptAny_opAssignInt_Generic), asCALL_GENERIC); assert( r >= 0 );

	// ORGLIN EXPERIMENT: implicit conversion OUT of `any` (auto-retrieve).
	// `opImplConv(?&out)` is the IMPLICIT generic form the compiler looks for when
	// an object is used where a primitive is expected (`float z = x`); `opConv` is
	// its explicit-only twin, kept so `double(x)` still works. The value-returning
	// opConv overloads mirror what the dictionary addon registers on dictionaryValue.
	r = engine->RegisterObjectMethod("any", "void opImplConv(?&out)", asFUNCTION(ScriptAny_opCast_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "void opConv(?&out)", asFUNCTION(ScriptAny_opCast_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "int64 opConv()", asFUNCTION(ScriptAny_opConvInt_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opConv()", asFUNCTION(ScriptAny_opConvDouble_Generic), asCALL_GENERIC); assert( r >= 0 );
	// The IMPLICIT fixed-return conversions. This is the pair the compiler's
	// object->primitive path searches for (ImplicitConvObjectToPrimitive accepts
	// only `opImplConv` with 0 parameters) — that is what makes `float z = x` work.
	r = engine->RegisterObjectMethod("any", "int64 opImplConv()", asFUNCTION(ScriptAny_opConvInt_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opImplConv()", asFUNCTION(ScriptAny_opConvDouble_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opAdd(double)", asFUNCTION(ScriptAny_opAdd_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opAdd(const int64&in)", asFUNCTION(ScriptAny_opAdd_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opSub(double)", asFUNCTION(ScriptAny_opSub_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opMul(double)", asFUNCTION(ScriptAny_opMul_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "double opDiv(double)", asFUNCTION(ScriptAny_opDiv_Generic), asCALL_GENERIC); assert( r >= 0 );

	r = engine->RegisterObjectBehaviour("any", asBEHAVE_ADDREF, "void f()", asFUNCTION(ScriptAny_AddRef_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_RELEASE, "void f()", asFUNCTION(ScriptAny_Release_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "any &opAssign(any&in)", asFUNCTION(ScriptAnyAssignment_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "void store(?&in)", asFUNCTION(ScriptAny_Store_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "void store(const int64&in)", asFUNCTION(ScriptAny_StoreInt_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "void store(const double&in)", asFUNCTION(ScriptAny_StoreFlt_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "bool retrieve(?&out) const", asFUNCTION(ScriptAny_Retrieve_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "bool retrieve(int64&out) const", asFUNCTION(ScriptAny_RetrieveInt_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectMethod("any", "bool retrieve(double&out) const", asFUNCTION(ScriptAny_RetrieveFlt_Generic), asCALL_GENERIC); assert( r >= 0 );

	// Register GC behaviours
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_GETREFCOUNT, "int f()", asFUNCTION(ScriptAny_GetRefCount_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_SETGCFLAG, "void f()", asFUNCTION(ScriptAny_SetFlag_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_GETGCFLAG, "bool f()", asFUNCTION(ScriptAny_GetFlag_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_ENUMREFS, "void f(int&in)", asFUNCTION(ScriptAny_EnumReferences_Generic), asCALL_GENERIC); assert( r >= 0 );
	r = engine->RegisterObjectBehaviour("any", asBEHAVE_RELEASEREFS, "void f(int&in)", asFUNCTION(ScriptAny_ReleaseAllHandles_Generic), asCALL_GENERIC); assert( r >= 0 );
}


CScriptAny &CScriptAny::operator=(const CScriptAny &other)
{
	// Hold on to the object type reference so it isn't destroyed too early
	if( (other.value.typeId & asTYPEID_MASK_OBJECT) )
	{
		asITypeInfo *ti = engine->GetTypeInfoById(other.value.typeId);
		if( ti )
			ti->AddRef();
	}

	FreeObject();

	value.typeId = other.value.typeId;
	if( value.typeId & asTYPEID_OBJHANDLE )
	{
		// For handles, copy the pointer and increment the reference count
		value.valueObj = other.value.valueObj;
		engine->AddRefScriptObject(value.valueObj, engine->GetTypeInfoById(value.typeId));
	}
	else if( value.typeId & asTYPEID_MASK_OBJECT )
	{
		// Create a copy of the object
		value.valueObj = engine->CreateScriptObjectCopy(other.value.valueObj, engine->GetTypeInfoById(value.typeId));
	}
	else
	{
		// Primitives can be copied directly
		value.valueInt = other.value.valueInt;
	}

	return *this;
}

int CScriptAny::CopyFrom(const CScriptAny *other)
{
	if( other == 0 ) return asINVALID_ARG;

	*this = *other;

	return 0;
}

CScriptAny::CScriptAny(asIScriptEngine *engine)
{
	this->engine = engine;
	refCount = 1;
	gcFlag = false;

	value.typeId = 0;
	value.valueInt = 0;

	// Notify the garbage collector of this object
	engine->NotifyGarbageCollectorOfNewObject(this, engine->GetTypeInfoByName("any"));
}

CScriptAny::CScriptAny(void *ref, int refTypeId, asIScriptEngine *engine)
{
	this->engine = engine;
	refCount = 1;
	gcFlag = false;

	value.typeId = 0;
	value.valueInt = 0;

	// Notify the garbage collector of this object
	engine->NotifyGarbageCollectorOfNewObject(this, engine->GetTypeInfoByName("any"));

	Store(ref, refTypeId);
}

CScriptAny::~CScriptAny()
{
	FreeObject();
}

void CScriptAny::Store(void *ref, int refTypeId)
{
	// This method is not expected to be used for primitive types, except for bool, int64, or double
	assert( refTypeId > asTYPEID_DOUBLE || refTypeId == asTYPEID_VOID || refTypeId == asTYPEID_BOOL || refTypeId == asTYPEID_INT64 || refTypeId == asTYPEID_DOUBLE );

	// Hold on to the object type reference so it isn't destroyed too early
	if( (refTypeId & asTYPEID_MASK_OBJECT) )
	{
		asITypeInfo *ti = engine->GetTypeInfoById(refTypeId);
		if( ti )
			ti->AddRef();
	}

	FreeObject();

	value.typeId = refTypeId;
	if( value.typeId & asTYPEID_OBJHANDLE )
	{
		// We're receiving a reference to the handle, so we need to dereference it
		value.valueObj = *(void**)ref;
		engine->AddRefScriptObject(value.valueObj, engine->GetTypeInfoById(value.typeId));
	}
	else if( value.typeId & asTYPEID_MASK_OBJECT )
	{
		// Create a copy of the object
		value.valueObj = engine->CreateScriptObjectCopy(ref, engine->GetTypeInfoById(value.typeId));
	}
	else
	{
		// Primitives can be copied directly
		value.valueInt = 0;

		// Copy the primitive value
		// We receive a pointer to the value.
		int size = engine->GetSizeOfPrimitiveType(value.typeId);
		memcpy(&value.valueInt, ref, size);
	}
}

void CScriptAny::Store(double &ref)
{
	Store(&ref, asTYPEID_DOUBLE);
}

void CScriptAny::Store(asINT64 &ref)
{
	Store(&ref, asTYPEID_INT64);
}


bool CScriptAny::Retrieve(void *ref, int refTypeId) const
{
	// This method is not expected to be used for primitive types, except for bool, int64, or double
	assert( refTypeId > asTYPEID_DOUBLE || refTypeId == asTYPEID_BOOL || refTypeId == asTYPEID_INT64 || refTypeId == asTYPEID_DOUBLE );

	if( refTypeId & asTYPEID_OBJHANDLE )
	{
		// Is the handle type compatible with the stored value?

		// A handle can be retrieved if the stored type is a handle of same or compatible type
		// or if the stored type is an object that implements the interface that the handle refer to.
		if( (value.typeId & asTYPEID_MASK_OBJECT) )
		{
			// Don't allow the retrieval if the stored handle is to a const object but not the wanted handle
			if( (value.typeId & asTYPEID_HANDLETOCONST) && !(refTypeId & asTYPEID_HANDLETOCONST) )
				return false;

			// RefCastObject will increment the refCount of the returned pointer if successful
			engine->RefCastObject(value.valueObj, engine->GetTypeInfoById(value.typeId), engine->GetTypeInfoById(refTypeId), reinterpret_cast<void**>(ref));
			if( *(asPWORD*)ref == 0 )
				return false;
			return true;
		}
	}
	else if( refTypeId & asTYPEID_MASK_OBJECT )
	{
		// Is the object type compatible with the stored value?

		// Copy the object into the given reference
		if( value.typeId == refTypeId )
		{
			engine->AssignScriptObject(ref, value.valueObj, engine->GetTypeInfoById(value.typeId));
			return true;
		}
	}
	else
	{
		// Is the primitive type compatible with the stored value?

		if( value.typeId == refTypeId )
		{
			int size = engine->GetSizeOfPrimitiveType(refTypeId);
			memcpy(ref, &value.valueInt, size);
			return true;
		}

		// We know all numbers are stored as either int64 or double, since we register overloaded functions for those
		if( value.typeId == asTYPEID_INT64 && refTypeId == asTYPEID_DOUBLE )
		{
			*(double*)ref = double(value.valueInt);
			return true;
		}
		else if( value.typeId == asTYPEID_DOUBLE && refTypeId == asTYPEID_INT64 )
		{
			*(asINT64*)ref = asINT64(value.valueFlt);
			return true;
		}
	}

	return false;
}

bool CScriptAny::Retrieve(asINT64 &outValue) const
{
	return Retrieve(&outValue, asTYPEID_INT64);
}

bool CScriptAny::Retrieve(double &outValue) const
{
	return Retrieve(&outValue, asTYPEID_DOUBLE);
}

int CScriptAny::GetTypeId() const
{
	return value.typeId;
}

void CScriptAny::FreeObject()
{
	// If it is a handle or a ref counted object, call release
	if( value.typeId & asTYPEID_MASK_OBJECT )
	{
		// Let the engine release the object
		asITypeInfo *ti = engine->GetTypeInfoById(value.typeId);
		engine->ReleaseScriptObject(value.valueObj, ti);

		// Release the object type info
		if( ti )
			ti->Release();

		value.valueObj = 0;
		value.typeId = 0;
	}

	// For primitives, there's nothing to do
}


void CScriptAny::EnumReferences(asIScriptEngine *inEngine)
{
	// If we're holding a reference, we'll notify the garbage collector of it
	if (value.valueObj && (value.typeId & asTYPEID_MASK_OBJECT))
	{
		asITypeInfo *subType = engine->GetTypeInfoById(value.typeId);
		if ((subType->GetFlags() & asOBJ_REF))
		{
			inEngine->GCEnumCallback(value.valueObj);
		}
		else if ((subType->GetFlags() & asOBJ_VALUE) && (subType->GetFlags() & asOBJ_GC))
		{
			// For value types we need to forward the enum callback
			// to the object so it can decide what to do
			engine->ForwardGCEnumReferences(value.valueObj, subType);
		}

		// The object type itself is also garbage collected
		asITypeInfo *ti = inEngine->GetTypeInfoById(value.typeId);
		if (ti)
			inEngine->GCEnumCallback(ti);
	}
}

void CScriptAny::ReleaseAllHandles(asIScriptEngine * /*engine*/)
{
	FreeObject();
}

int CScriptAny::AddRef() const
{
	// Increase counter and clear flag set by GC
	gcFlag = false;
	return asAtomicInc(refCount);
}

int CScriptAny::Release() const
{
	// Decrease the ref counter
	gcFlag = false;
	if( asAtomicDec(refCount) == 0 )
	{
		// Delete this object as no more references to it exists
		delete this;
		return 0;
	}

	return refCount;
}

int CScriptAny::GetRefCount()
{
	return refCount;
}

void CScriptAny::SetFlag()
{
	gcFlag = true;
}

bool CScriptAny::GetFlag()
{
	return gcFlag;
}


END_AS_NAMESPACE
