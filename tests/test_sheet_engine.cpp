// Sheet engine tests (Linux, portable): formula evaluation vs documented Excel behaviour,
// number formats, input parsing, reference adjustment, recalculation, undo, clipboard,
// XLSX / ODS round trips (verified with python openpyxl / LibreOffice when present),
// DEFLATE round trips vs zlib, performance.
#include "core/sheet.h"
#include "core/fileio.h"
#include "io/deflate.h"
#include "io/inflate.h"
#include "io/ods.h"
#include "io/xlsx.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

using namespace pcbp;
using namespace pcbp::sheet;
static int g_fail = 0, g_ok = 0;
#define CHECK(c) do { if (c) ++g_ok; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
static std::string tos(const std::string& s) { return s; }
static std::string tos(int v) { return std::to_string(v); }
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (_a == _b) ++g_ok; else { ++g_fail; std::printf("FAIL %s:%d  %s == %s  ('%s' vs '%s')\n", __FILE__, __LINE__, #a, #b, tos(_a).c_str(), tos(_b).c_str()); } } while (0)

static bool same_text(const std::string& got, const std::string& exp) {
    if (got == exp) return true;
    char* e1 = nullptr; char* e2 = nullptr;
    double a = strtod(got.c_str(), &e1), b = strtod(exp.c_str(), &e2);
    if (!got.empty() && !exp.empty() && *e1 == 0 && *e2 == 0) return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b));
    return false;
}

static Book fixture() {
    Book b;
    b.rename_sheet(0, "Sheet1");
    b.add_sheet("Data 2");
    std::vector<std::vector<std::string>> blk = {
        {"1", "R1", "10", "apple", "0", "R1-R5,R7"},
        {"2", "R2", "20", "Banana", "", "C1, C2"},
        {"3", "C1", "x", "cherry"},
        {"4", "C2", "TRUE"},
        {"5", "U1"},
    };
    b.set_inputs(0, 0, 0, blk);
    b.set_input(1, 0, 0, "100");
    b.set_input(1, 1, 0, "200");
    b.define_name({"Nums", -1, "Sheet1!$A$1:$A$5", false});
    b.clear_undo();
    return b;
}

static void formula_suite() {
    Book b = fixture();
    int n_cases = 0;
    auto T = [&](const char* f, const std::string& exp) {
        ++n_cases;
        Value v = b.evaluate(f, 0, 19, 7);   // host H20
        std::string got = value_to_text(v);
        if (v.is_empty()) got = "";
        if (same_text(got, exp)) ++g_ok;
        else { ++g_fail; std::printf("FAIL formula =%s  -> '%s' (expected '%s')\n", f, got.c_str(), exp.c_str()); }
    };
    // ---- operators & coercion ----
    T("1+2", "3"); T("2*3+4", "10"); T("2+3*4", "14"); T("(2+3)*4", "20"); T("2^3", "8"); T("2^3^2", "64");
    T("-2^2", "4"); T("-(2^2)", "-4"); T("10/4", "2.5"); T("1/0", "#DIV/0!"); T("5%", "0.05"); T("50%*2", "1");
    T("\"a\"&\"b\"", "ab"); T("1&2", "12"); T("TRUE+1", "2"); T("\"3\"+4", "7"); T("\"abc\"+1", "#VALUE!");
    T("1=1", "TRUE"); T("1<>1", "FALSE"); T("\"a\"=\"A\"", "TRUE"); T("\"a\"<\"b\"", "TRUE"); T("1<\"a\"", "TRUE");
    T("\"z\"<TRUE", "TRUE"); T("FALSE<TRUE", "TRUE"); T("A1+A2", "3"); T("A1:A3*2", "2"); T("SUM(A1:A3*2)", "12");
    T("E1=0", "TRUE"); T("G1=0", "TRUE"); T("G1=\"\"", "TRUE"); T("G1+1", "1"); T("#N/A+1", "#N/A"); T("1/(1-1)", "#DIV/0!");
    T("0^0", "#NUM!"); T("(-8)^(1/3)", "-2"); T("(-8)^0.5", "#NUM!"); T("3-2-1", "0"); T("2*-3", "-6"); T("--\"5\"", "5");
    T("1+\"1,000\"", "1001"); T("\"2024-01-15\"+0", "45306"); T("1e3+1", "1001"); T("10%%", "0.001");
    T("SUM(A1:A3 A2:A5)", "5"); T("SUM((A1,A3))", "4"); T("ROWS(A1:A3 B1:B2)", "#NULL!"); T("1+#REF!", "#REF!");
    T("\"a\"&1/0", "#DIV/0!"); T("0.1+0.2=0.3", "TRUE"); T("2^-1", "0.5"); T("1E+300*1E+300", "#NUM!");
    // ---- math ----
    T("SUM(A1:A5)", "15"); T("SUM(A1:A5,10)", "25"); T("SUM(C1:C5)", "30"); T("SUM(\"3\",2)", "5"); T("SUM(TRUE,1)", "2");
    T("SUM(\"x\")", "#VALUE!"); T("SUM(A:A)", "15"); T("SUM(1/0)", "#DIV/0!"); T("SUMPRODUCT(A1:A3,A1:A3)", "14");
    T("SUMPRODUCT((A1:A5>2)*1)", "3"); T("SUMPRODUCT(A1:A2,A1:A3)", "#VALUE!"); T("SUMSQ(1,2,3)", "14"); T("PRODUCT(A1:A4)", "24");
    T("ROUND(2.5,0)", "3"); T("ROUND(-2.5,0)", "-3"); T("ROUND(2.675,2)", "2.68"); T("ROUND(1234.567,-2)", "1200"); T("ROUND(0.5,0)", "1");
    T("ROUNDUP(3.2,0)", "4"); T("ROUNDUP(-3.2,0)", "-4"); T("ROUNDDOWN(3.9,0)", "3"); T("ROUNDDOWN(-3.9,0)", "-3"); T("ROUNDUP(3.14159,3)", "3.142");
    T("INT(-3.5)", "-4"); T("INT(3.9)", "3"); T("TRUNC(-3.9)", "-3"); T("TRUNC(3.14159,2)", "3.14"); T("MOD(10,3)", "1"); T("MOD(-10,3)", "2");
    T("MOD(10,-3)", "-2"); T("MOD(5,0)", "#DIV/0!"); T("MOD(5.5,2)", "1.5"); T("ABS(-4)", "4"); T("SIGN(-0.5)", "-1"); T("SQRT(16)", "4");
    T("SQRT(-1)", "#NUM!"); T("POWER(2,10)", "1024"); T("EXP(0)", "1"); T("LN(EXP(2))", "2"); T("LOG(100)", "2"); T("LOG(8,2)", "3");
    T("LOG10(1000)", "3"); T("LOG(0)", "#NUM!"); T("PI()", "3.14159265358979"); T("FACT(5)", "120"); T("FACT(0)", "1"); T("COMBIN(5,2)", "10");
    T("PERMUT(5,2)", "20"); T("GCD(12,18)", "6"); T("LCM(4,6)", "12"); T("EVEN(3)", "4"); T("EVEN(-1)", "-2"); T("ODD(2)", "3"); T("ODD(-2)", "-3");
    T("QUOTIENT(7,2)", "3"); T("QUOTIENT(-7,2)", "-3"); T("MROUND(10,3)", "9"); T("MROUND(-10,-3)", "-9"); T("MROUND(5,-2)", "#NUM!");
    T("CEILING(2.5,1)", "3"); T("CEILING(-2.5,-2)", "-4"); T("CEILING(2.5,-1)", "#NUM!"); T("FLOOR(2.5,1)", "2"); T("FLOOR(-2.5,-2)", "-2");
    T("CEILING.MATH(-2.5)", "-2"); T("CEILING.MATH(-2.5,1,1)", "-3"); T("FLOOR.MATH(-2.5)", "-3"); T("FLOOR.MATH(2.5,2)", "2");
    T("SIN(0)", "0"); T("COS(0)", "1"); T("DEGREES(PI())", "180"); T("RADIANS(180)", "3.14159265358979"); T("ATAN2(1,1)", "0.785398163397448");
    T("SUBTOTAL(9,A1:A5)", "15"); T("SUBTOTAL(1,A1:A5)", "3"); T("SUBTOTAL(3,B1:B5)", "5"); T("SUBTOTAL(2,C1:C5)", "2");
    T("SUBTOTAL(109,A1:A5)", "15"); T("SUBTOTAL(4,A1:A5)", "5"); T("RANDBETWEEN(5,5)", "5"); T("RAND()<1", "TRUE"); T("SUM(SEQUENCE(4))", "10");
    T("INDEX(SEQUENCE(2,3,10,5),2,3)", "35"); T("SUM()", "#NAME?");
    // ---- statistical ----
    T("AVERAGE(A1:A5)", "3"); T("AVERAGE(C1:C5)", "15"); T("AVERAGE(G1:G3)", "#DIV/0!"); T("AVERAGEA(C1:C4)", "7.75");
    T("AVERAGEIF(A1:A5,\">2\")", "4"); T("AVERAGEIF(B1:B5,\"R*\",A1:A5)", "1.5"); T("AVERAGEIFS(A1:A5,A1:A5,\">1\",A1:A5,\"<5\")", "3");
    T("COUNT(A1:A5)", "5"); T("COUNT(C1:C5)", "2"); T("COUNT(1,\"2\",\"x\",TRUE)", "3"); T("COUNTA(C1:C5)", "4"); T("COUNTA(1,\"\",TRUE)", "3");
    T("COUNTBLANK(C1:C5)", "1"); T("COUNTIF(A1:A5,\">2\")", "3"); T("COUNTIF(B1:B5,\"R*\")", "2"); T("COUNTIF(B1:B5,\"r?\")", "2");
    T("COUNTIF(B1:B5,\"<>R1\")", "4"); T("COUNTIF(D1:D3,\"banana\")", "1"); T("COUNTIF(A1:A5,3)", "1"); T("COUNTIF(A1:A5,\"3\")", "1");
    T("COUNTIF(C1:C5,\"\")", "1"); T("COUNTIF(C1:C5,\"<>\")", "4"); T("COUNTIF(C1:C5,TRUE)", "1"); T("COUNTIF(A1:A5,\">=\"&A4)", "2");
    T("COUNTIFS(A1:A5,\">1\",B1:B5,\"C*\")", "2"); T("COUNTIFS(A1:A5,\">1\",B1:B3,\"C*\")", "#VALUE!"); T("MIN(A1:A5)", "1"); T("MAX(A1:A5)", "5");
    T("MIN(C1:C5)", "10"); T("MAX(G1:G3)", "0"); T("MINA(C1:C4)", "0"); T("MAXA(C3:C4)", "1"); T("MINIFS(A1:A5,B1:B5,\"C*\")", "3");
    T("MAXIFS(A1:A5,B1:B5,\"R*\")", "2"); T("MEDIAN(A1:A5)", "3"); T("MEDIAN(1,2,3,4)", "2.5"); T("MODE(1,2,2,3)", "2"); T("MODE(1,2,3)", "#N/A");
    T("LARGE(A1:A5,2)", "4"); T("SMALL(A1:A5,2)", "2"); T("LARGE(A1:A5,6)", "#NUM!"); T("RANK(3,A1:A5)", "3"); T("RANK(3,A1:A5,1)", "3");
    T("RANK(5,A1:A5)", "1"); T("RANK(6,A1:A5)", "#N/A"); T("STDEV(A1:A5)", "1.58113883008419"); T("STDEVP(A1:A5)", "1.4142135623731");
    T("VAR(A1:A5)", "2.5"); T("VARP(A1:A5)", "2"); T("STDEV(1)", "#DIV/0!"); T("PERCENTILE(A1:A5,0.25)", "2"); T("PERCENTILE(A1:A5,0.1)", "1.4");
    T("QUARTILE(A1:A5,3)", "4"); T("PERCENTILE.EXC(A1:A5,0.5)", "3"); T("SUMIF(A1:A5,\">2\")", "12"); T("SUMIF(B1:B5,\"R*\",A1:A5)", "3");
    T("SUMIF(B1:B5,\"C1\",A1:A5)", "3"); T("SUMIFS(A1:A5,B1:B5,\"C*\",A1:A5,\">3\")", "4"); T("SUMIF(A1:A5,\"<>3\")", "12");
    T("SUMIF(D1:D3,\"*an*\",A1:A3)", "2"); T("SUMIF(A:A,\">3\")", "9"); T("COUNTIF(A:A,\">0\")", "5"); T("STDEV.S(A1:A5)", "1.58113883008419");
    // ---- logical ----
    T("IF(TRUE,1,2)", "1"); T("IF(FALSE,1,2)", "2"); T("IF(FALSE,1)", "FALSE"); T("IF(TRUE,,2)", "0"); T("IF(A1>0,\"pos\",\"neg\")", "pos");
    T("IF(\"x\",1,2)", "#VALUE!"); T("IF(1/0,1,2)", "#DIV/0!"); T("IFS(A1>5,\"a\",A1>0,\"b\")", "b"); T("IFS(FALSE,1)", "#N/A");
    T("IFERROR(1/0,\"err\")", "err"); T("IFERROR(5,\"err\")", "5"); T("IFNA(NA(),\"na\")", "na"); T("IFNA(1/0,\"na\")", "#DIV/0!");
    T("AND(TRUE,1)", "TRUE"); T("AND(TRUE,0)", "FALSE"); T("AND(C1:C5)", "TRUE"); T("OR(FALSE,0)", "FALSE"); T("OR(A1:A5>4)", "TRUE");
    T("XOR(TRUE,TRUE)", "FALSE"); T("XOR(TRUE,FALSE,TRUE,TRUE)", "TRUE"); T("NOT(TRUE)", "FALSE"); T("NOT(0)", "TRUE"); T("AND(\"x\")", "#VALUE!");
    T("SWITCH(2,1,\"a\",2,\"b\",\"c\")", "b"); T("SWITCH(9,1,\"a\",\"c\")", "c"); T("SWITCH(9,1,\"a\")", "#N/A"); T("TRUE()", "TRUE");
    T("AND(G1:G3)", "#VALUE!"); T("IF(A1:A3>1,\"y\",\"n\")", "n"); T("SUM(IF(A1:A5>2,A1:A5,0))", "12");
    // ---- lookup & reference ----
    T("VLOOKUP(3,A1:B5,2,FALSE)", "C1"); T("VLOOKUP(3.5,A1:B5,2)", "C1"); T("VLOOKUP(3.5,A1:B5,2,TRUE)", "C1"); T("VLOOKUP(0,A1:B5,2)", "#N/A");
    T("VLOOKUP(9,A1:B5,2,FALSE)", "#N/A"); T("VLOOKUP(3,A1:B5,3,FALSE)", "#REF!"); T("VLOOKUP(3,A1:B5,0,FALSE)", "#VALUE!");
    T("VLOOKUP(\"c*\",D1:D3,1,FALSE)", "cherry"); T("VLOOKUP(\"BANANA\",D1:D3,1,FALSE)", "Banana"); T("VLOOKUP(99,A1:B5,2)", "U1");
    T("HLOOKUP(2,{1,2,3;\"a\",\"b\",\"c\"},2,FALSE)", "b"); T("HLOOKUP(2.5,{1,2,3;\"a\",\"b\",\"c\"},2)", "b");
    T("XLOOKUP(\"C2\",B1:B5,A1:A5)", "4"); T("XLOOKUP(\"zz\",B1:B5,A1:A5,\"none\")", "none"); T("XLOOKUP(\"zz\",B1:B5,A1:A5)", "#N/A");
    T("XLOOKUP(3.5,A1:A5,B1:B5,,-1)", "C1"); T("XLOOKUP(3.5,A1:A5,B1:B5,,1)", "C2"); T("XLOOKUP(\"R*\",B1:B5,A1:A5,,2)", "1");
    T("XLOOKUP(\"R*\",B1:B5,A1:A5,,2,-1)", "2"); T("XLOOKUP(4,A1:A5,A1:A5,,0,2)", "4"); T("INDEX(A1:B5,3,2)", "C1"); T("INDEX(A1:A5,4)", "4");
    T("SUM(INDEX(A1:B5,0,1))", "15"); T("INDEX({1,2;3,4},2,2)", "4"); T("INDEX(A1:A5,6)", "#REF!"); T("INDEX((A1:A2,B1:B2),1,1,2)", "R1");
    T("SUM(A1:INDEX(A1:A5,3))", "6"); T("MATCH(3,A1:A5,0)", "3"); T("MATCH(3.5,A1:A5)", "3"); T("MATCH(3.5,A1:A5,1)", "3");
    T("MATCH(\"banana\",D1:D3,0)", "2"); T("MATCH(\"b*\",D1:D3,0)", "2"); T("MATCH(9,A1:A5,0)", "#N/A"); T("MATCH(4,{5,4,3,2,1},-1)", "2");
    T("MATCH(3.5,{5,4,3,2,1},-1)", "2"); T("XMATCH(4,A1:A5)", "4"); T("XMATCH(3.5,A1:A5,1)", "4"); T("XMATCH(3.5,A1:A5,-1)", "3");
    T("CHOOSE(2,\"a\",\"b\",\"c\")", "b"); T("CHOOSE(4,\"a\",\"b\")", "#VALUE!"); T("SUM(CHOOSE(1,A1:A2,A3:A5))", "3"); T("OFFSET(A1,2,0)", "3");
    T("SUM(OFFSET(A1,1,0,3,1))", "9"); T("OFFSET(A1,-1,0)", "#REF!"); T("INDIRECT(\"A3\")", "3"); T("INDIRECT(\"'Data 2'!A2\")", "200");
    T("SUM(INDIRECT(\"A1:A\"&3))", "6"); T("INDIRECT(\"R2C1\",FALSE)", "2"); T("INDIRECT(\"bad!!\")", "#REF!"); T("SUM(INDIRECT(\"Nums\"))", "15");
    T("ROW(A7)", "7"); T("ROW()", "20"); T("COLUMN()", "8"); T("COLUMN(C1)", "3"); T("ROWS(A1:B5)", "5"); T("COLUMNS(A1:B5)", "2"); T("ROWS({1,2;3,4})", "2");
    T("ADDRESS(2,3)", "$C$2"); T("ADDRESS(2,3,4)", "C2"); T("ADDRESS(2,3,1,FALSE)", "R2C3"); T("ADDRESS(1,1,1,TRUE,\"My Sheet\")", "'My Sheet'!$A$1");
    T("LOOKUP(3.5,A1:A5,B1:B5)", "C1"); T("LOOKUP(2,{1,2,3},{\"a\",\"b\",\"c\"})", "b"); T("SUM(TRANSPOSE(A1:A3))", "6"); T("COLUMNS(TRANSPOSE(A1:A3))", "3");
    T("ROWS(UNIQUE({1;1;2}))", "2"); T("ROWS(UNIQUE({1;2;2;3},,TRUE))", "2"); T("INDEX(SORT({3;1;2}),1)", "1"); T("INDEX(SORT({3;1;2},1,-1),1)", "3");
    T("INDEX(SORT({\"R10\";\"R2\";\"R1\"}),2)", "R2"); T("ROWS(FILTER(A1:A5,A1:A5>2))", "3"); T("FILTER(A1:A5,A1:A5>9,\"none\")", "none");
    T("FILTER(A1:A5,A1:A5>9)", "#CALC!"); T("INDEX(SORTBY(B1:B5,A1:A5,-1),1)", "U1"); T("SUM(FILTER(A1:A5,(A1:A5>1)*(A1:A5<5)))", "9");
    T("ROWS(UNIQUE(B1:B5))", "5"); T("HYPERLINK(\"http://x\",\"go\")", "go");
    // ---- text ----
    T("LEFT(\"hello\",2)", "he"); T("LEFT(\"hello\")", "h"); T("LEFT(\"hi\",5)", "hi"); T("LEFT(\"x\",-1)", "#VALUE!"); T("RIGHT(\"hello\",3)", "llo");
    T("MID(\"hello\",2,3)", "ell"); T("MID(\"hello\",10,2)", ""); T("MID(\"hello\",0,1)", "#VALUE!"); T("LEN(\"hello\")", "5");
    T("LEN(\"\xE6\x97\xA5\xE6\x9C\xAC\")", "2"); T("LEN(123)", "3"); T("LEN(\"\")", "0"); T("FIND(\"l\",\"hello\")", "3"); T("FIND(\"L\",\"hello\")", "#VALUE!");
    T("FIND(\"l\",\"hello\",4)", "4"); T("SEARCH(\"L\",\"hello\")", "3"); T("SEARCH(\"l?o\",\"hello\")", "3"); T("SEARCH(\"*\",\"hello\")", "1");
    T("SEARCH(\"z\",\"hello\")", "#VALUE!"); T("SUBSTITUTE(\"a-b-c\",\"-\",\"+\")", "a+b+c"); T("SUBSTITUTE(\"a-b-c\",\"-\",\"+\",2)", "a-b+c");
    T("SUBSTITUTE(\"aaa\",\"a\",\"b\",4)", "aaa"); T("REPLACE(\"abcdef\",2,3,\"X\")", "aXef"); T("TRIM(\"  a   b  \")", "a b");
    T("CLEAN(\"a\"&CHAR(7)&\"b\")", "ab"); T("UPPER(\"abc\")", "ABC"); T("LOWER(\"\xC3\x84" "BC\")", "\xC3\xA4" "bc"); T("PROPER(\"hello world-foo\")", "Hello World-Foo");
    T("PROPER(\"o'neil\")", "O'Neil"); T("TEXT(1234.567,\"#,##0.00\")", "1,234.57"); T("TEXT(0.5,\"0%\")", "50%"); T("TEXT(45306,\"yyyy-mm-dd\")", "2024-01-15");
    T("TEXT(45306,\"dddd\")", "Monday"); T("TEXT(45306,\"mmm d, yyyy\")", "Jan 15, 2024"); T("TEXT(0.75,\"h:mm AM/PM\")", "6:00 PM"); T("TEXT(1.5,\"[h]:mm\")", "36:00");
    T("TEXT(-5,\"0;(0)\")", "(5)"); T("TEXT(0,\"0;-0;\"\"zero\"\"\")", "zero"); T("TEXT(12,\"000\")", "012"); T("TEXT(1234567,\"0.00E+00\")", "1.23E+06");
    T("TEXT(1.25,\"# ?/?\")", "1 1/4"); T("TEXT(\"abc\",\"@\")", "abc"); T("TEXT(5,\"\"\"Qty: \"\"0\")", "Qty: 5"); T("TEXT(1234.5,\"$#,##0.00\")", "$1,234.50");
    T("TEXT(0.1234,\"0.0%\")", "12.3%"); T("TEXT(1500000,\"#,##0,,\"\"M\"\"\")", "2M"); T("TEXT(3.14159,\"0.00\")", "3.14"); T("TEXT(1234,\"#,##0\")", "1,234");
    T("TEXT(5551234,\"000-0000\")", "555-1234"); T("TEXT(0.5,\"0.0\")", "0.5"); T("TEXT(45306.5,\"m/d/yyyy h:mm\")", "1/15/2024 12:00");
    T("VALUE(\"123\")", "123"); T("VALUE(\"1,234.5\")", "1234.5"); T("VALUE(\"50%\")", "0.5"); T("VALUE(\"abc\")", "#VALUE!"); T("VALUE(\"$10\")", "10");
    T("NUMBERVALUE(\"1.234,5\",\",\",\".\")", "1234.5"); T("CONCAT(\"a\",\"b\",\"c\")", "abc"); T("CONCAT(A1:A3)", "123"); T("CONCATENATE(\"a\",1,TRUE)", "a1TRUE");
    T("TEXTJOIN(\",\",TRUE,\"a\",\"\",\"b\")", "a,b"); T("TEXTJOIN(\",\",FALSE,\"a\",\"\",\"b\")", "a,,b"); T("TEXTJOIN(\"-\",TRUE,A1:A3)", "1-2-3");
    T("EXACT(\"a\",\"A\")", "FALSE"); T("EXACT(\"a\",\"a\")", "TRUE"); T("REPT(\"ab\",3)", "ababab"); T("REPT(\"a\",-1)", "#VALUE!"); T("CHAR(65)", "A");
    T("CODE(\"A\")", "65"); T("UNICHAR(9731)", "\xE2\x98\x83"); T("UNICODE(\"\xE2\x82\xAC\")", "8364"); T("CODE(\"\xE2\x82\xAC\")", "128"); T("FIXED(1234.567,1)", "1,234.6");
    T("FIXED(1234.567,1,TRUE)", "1234.6"); T("DOLLAR(1234.567)", "$1,234.57"); T("T(\"a\")", "a"); T("T(1)", ""); T("N(TRUE)", "1"); T("N(\"a\")", "0");
    T("TEXTBEFORE(\"a-b-c\",\"-\")", "a"); T("TEXTAFTER(\"a-b-c\",\"-\",-1)", "c"); T("TEXTBEFORE(\"abc\",\"x\")", "#N/A"); T("INDEX(TEXTSPLIT(\"a,b,c\",\",\"),1,2)", "b");
    T("COLUMNS(TEXTSPLIT(\"a,b,c\",\",\"))", "3"); T("ROWS(TEXTSPLIT(\"a,b;c,d\",\",\",\";\"))", "2"); T("LEFT(B1)&RIGHT(B3)", "R1"); T("LEN(A1:A3)", "1");
    T("UPPER(D1:D3)", "APPLE"); T("\"x\"&G1&\"y\"", "xy");
    // ---- date & time ----
    T("DATE(2024,1,15)", "45306"); T("DATE(2024,13,1)", "45658"); T("DATE(2024,1,0)", "45291"); T("DATE(100,1,1)", "36526"); T("DATE(1900,1,1)", "1");
    T("DATE(1900,3,1)", "61"); T("DATE(1900,2,28)", "59"); T("YEAR(45306)", "2024"); T("MONTH(45306)", "1"); T("DAY(45306)", "15"); T("YEAR(60)", "1900");
    T("MONTH(60)", "2"); T("DAY(60)", "29"); T("WEEKDAY(45306)", "2"); T("WEEKDAY(45306,2)", "1"); T("WEEKDAY(45306,3)", "0"); T("WEEKDAY(1)", "1");
    T("WEEKNUM(45306)", "3"); T("WEEKNUM(45306,2)", "3"); T("ISOWEEKNUM(45306)", "3"); T("HOUR(0.75)", "18"); T("MINUTE(0.7604166667)", "15");
    T("SECOND(TIME(1,2,3))", "3"); T("TIME(12,0,0)", "0.5"); T("TIME(25,0,0)", "0.0416666666666667");
    T("DATEDIF(DATE(2020,1,15),DATE(2024,3,10),\"Y\")", "4"); T("DATEDIF(DATE(2020,1,15),DATE(2024,3,10),\"M\")", "49");
    T("DATEDIF(DATE(2020,1,15),DATE(2024,3,10),\"D\")", "1516"); T("DATEDIF(DATE(2020,1,15),DATE(2024,3,10),\"YM\")", "1");
    T("DATEDIF(DATE(2020,1,15),DATE(2024,3,10),\"MD\")", "24"); T("DATEDIF(DATE(2020,1,15),DATE(2024,3,10),\"YD\")", "55");
    T("DATEDIF(DATE(2024,1,1),DATE(2023,1,1),\"D\")", "#NUM!"); T("EDATE(DATE(2024,1,31),1)", "45351"); T("EOMONTH(DATE(2024,1,15),1)", "45351");
    T("EOMONTH(DATE(2024,1,15),-1)", "45291"); T("DAYS(DATE(2024,3,1),DATE(2024,2,1))", "29"); T("NETWORKDAYS(DATE(2024,1,1),DATE(2024,1,31))", "23");
    T("WORKDAY(DATE(2024,1,5),1)", "45299"); T("DATEVALUE(\"2024-01-15\")", "45306"); T("DATEVALUE(\"1/15/2024\")", "45306"); T("TIMEVALUE(\"12:00\")", "0.5");
    T("YEARFRAC(DATE(2024,1,1),DATE(2024,7,1))", "0.5"); T("YEARFRAC(DATE(2023,1,1),DATE(2024,1,1),3)", "1"); T("TODAY()>45000", "TRUE");
    T("NOW()>=TODAY()", "TRUE"); T("MONTH(DATE(2024,2,30))", "3"); T("DATEVALUE(\"abc\")", "#VALUE!"); T("YEAR(-1)", "#NUM!");
    T("NETWORKDAYS(DATE(2024,1,1),DATE(2024,1,5),DATE(2024,1,1))", "4"); T("DATE(2024,1,15)-DATE(2023,1,15)", "365");
    // ---- information ----
    T("ISBLANK(G1)", "TRUE"); T("ISBLANK(A1)", "FALSE"); T("ISBLANK(\"\")", "FALSE"); T("ISNUMBER(A1)", "TRUE"); T("ISNUMBER(\"1\")", "FALSE");
    T("ISTEXT(B1)", "TRUE"); T("ISNONTEXT(A1)", "TRUE"); T("ISLOGICAL(C4)", "TRUE"); T("ISERROR(1/0)", "TRUE"); T("ISERROR(NA())", "TRUE");
    T("ISERR(NA())", "FALSE"); T("ISNA(NA())", "TRUE"); T("ISREF(A1)", "TRUE"); T("ISREF(1)", "FALSE"); T("ISEVEN(4)", "TRUE"); T("ISODD(3.7)", "TRUE");
    T("ISFORMULA(A1)", "FALSE"); T("NA()", "#N/A"); T("ERROR.TYPE(1/0)", "2"); T("ERROR.TYPE(NA())", "7"); T("ERROR.TYPE(1)", "#N/A"); T("TYPE(1)", "1");
    T("TYPE(\"a\")", "2"); T("TYPE(TRUE)", "4"); T("TYPE(1/0)", "16"); T("TYPE({1,2})", "64"); T("SHEET()", "1"); T("SHEETS()", "2"); T("N(A1)", "1");
    T("ISNUMBER(A1:A3)", "TRUE"); T("ISERROR(#VALUE!)", "TRUE"); T("ISTEXT(C3)", "TRUE");
    // ---- BOM helpers ----
    T("REFCOUNT(\"R1-R5,R7\")", "6"); T("REFCOUNT(\"R1, R2 R3;R4\")", "4"); T("REFCOUNT(\"\")", "0"); T("REFCOUNT(F1:F2)", "8");
    T("REFEXPAND(\"R1-R3\")", "R1, R2, R3"); T("REFEXPAND(\"R1-R3\",\",\")", "R1,R2,R3"); T("REFCOMPRESS(\"R3,R1,R2,R5\")", "R1-R3, R5");
    T("REFCOMPRESS(\"C1,C2\")", "C1, C2"); T("REFCOUNT(\"U1-U12\")", "12"); T("REFEXPAND(\"R1-5\")", "R1, R2, R3, R4, R5"); T("REFCOUNT(\"J1-J3, TP1\")", "4");
    T("REFCOMPRESS(REFEXPAND(\"R1-R10\"))", "R1-R10"); T("REFCOUNT(\"R1 - R4\")", "4");
    // ---- names, sheets, arrays, misc ----
    T("SUM(Nums)", "15"); T("AVERAGE(Nums)", "3"); T("ROWS(Nums)", "5"); T("UndefinedName", "#NAME?"); T("FOO(1)", "#NAME?"); T("nums", "1");
    T("'Data 2'!A1", "100"); T("SUM('Data 2'!A1:A2)", "300"); T("'Data 2'!A1+A1", "101"); T("SUM('Sheet1:Data 2'!A1)", "101");
    T("SUM({1,2,3})", "6"); T("SUM({1,2;3,4})", "10"); T("INDEX({1,2,3},2)", "2"); T("ROWS({1;2;3})", "3"); T("{1,2,3}*2", "2");
    T("SUM({1,2,3}*{4,5,6})", "32"); T("AND({TRUE,FALSE})", "FALSE"); T("MAX({1,5,3})", "5"); T("SUM(({1,2,3}>1)*1)", "2"); T("#REF!", "#REF!");
    T("SUM(A1:A2,B1:B2,{1})", "4"); T("_xlfn.CONCAT(\"a\",\"b\")", "ab"); T("sum(a1:a2)", "3"); T("SUM( A1 , A2 )", "3"); T("A1#", "#REF!");
    std::printf("formula cases: %d\n", n_cases);
}

static void format_suite() {
    auto F = [](double v, const char* code, const char* exp) {
        std::string got = format_value(Value::number(v), code).text;
        if (got == exp) ++g_ok; else { ++g_fail; std::printf("FAIL format %g '%s' -> '%s' (expected '%s')\n", v, code, got.c_str(), exp); }
    };
    F(1234.5, "General", "1234.5"); F(1.0 / 3, "General", "0.333333333"); F(123456789012.0, "General", "1.23457E+11"); F(0.1, "General", "0.1");
    F(-1234.567, "#,##0.00", "-1,234.57"); F(0, "0.00", "0.00"); F(0.5, "0%", "50%"); F(12345.678, "0.00E+00", "1.23E+04"); F(1e-5, "0.00E+00", "1.00E-05");
    F(45306, "m/d/yyyy", "1/15/2024"); F(45306, "d-mmm-yy", "15-Jan-24"); F(45306.75, "h:mm AM/PM", "6:00 PM"); F(0.5, "h:mm:ss", "12:00:00");
    F(1.25, "[h]:mm:ss", "30:00:00"); F(-5, "#,##0_);(#,##0)", "(5)"); F(5, "#,##0_);(#,##0)", "5 "); F(-5, "#,##0_);[Red](#,##0)", "(5)");
    F(0.25, "# ?/?", " 1/4"); F(2.5, "# ??/??", "2  1/2 "); F(1234, "\"Qty \"0", "Qty 1234"); F(3, "0.0#", "3.0"); F(3.14159, "0.0#", "3.14");
    F(42, "00000", "00042"); F(1500, "#,##0.0,\"k\"", "1.5k"); F(45306, "dddd, mmmm d", "Monday, January 15"); F(0.000694444444, "mm:ss", "01:00");
    F(5, "[<10]\"low\";\"high\"", "low"); F(50, "[<10]\"low\";\"high\"", "high"); F(45306.5, "yyyy-mm-dd hh:mm:ss", "2024-01-15 12:00:00");
    F(0.1234, "0.00%", "12.34%"); F(1, "\"$\"#,##0.00", "$1.00"); F(1e6, "#,##0", "1,000,000"); F(-0.0001, "0.00", "0.00");
    F(12, "[$\xE2\x82\xAC-407]#,##0.00", "\xE2\x82\xAC" "12.00"); F(0.000011574, "ss.000", "01.000"); F(60, "m/d/yyyy", "2/29/1900");
    Formatted red = format_value(Value::number(-1), "0;[Red]-0");
    CHECK(red.color == 0xFFFF0000 && red.text == "-1");
    CHECK_EQ(format_value(Value::text("ab"), "\"<\"@\">\"").text, std::string("<ab>"));
    CHECK_EQ(format_value(Value::text("ab"), "0.00;-0.00;0;\"t:\"@").text, std::string("t:ab"));
    CHECK(is_date_format("m/d/yyyy") && is_date_format("[h]:mm") && !is_date_format("0.00") && !is_date_format("\"m\"0"));
    CHECK_EQ(builtin_numfmt(14), std::string("m/d/yyyy"));
    CHECK(builtin_numfmt_id("0.00%") == 10);
    // input parsing
    std::string h;
    CHECK(parse_input("12").num == 12);
    CHECK(parse_input("0805").is_str());
    CHECK(parse_input("'123").is_str() && parse_input("'123").str == "123");
    Value p = parse_input("12%", &h); CHECK(p.num == 0.12 && h == "0%");
    p = parse_input("$1,234.50", &h); CHECK(p.num == 1234.5 && h == "\"$\"#,##0.00");
    p = parse_input("1,234", &h); CHECK(p.num == 1234 && h == "#,##0");
    CHECK(parse_input("1,23").is_str());
    p = parse_input("2024-03-15", &h); CHECK(p.num == 45366 && is_date_format(h));
    p = parse_input("3/15/2024", &h); CHECK(p.num == 45366);
    p = parse_input("13:45", &h); CHECK(std::fabs(p.num - (13 * 60 + 45) / 1440.0) < 1e-12 && h == "h:mm");
    p = parse_input("1:30 PM", &h); CHECK(std::fabs(p.num - 13.5 / 24) < 1e-12);
    CHECK(parse_input("TRUE").is_bool() && parse_input("true").b());
    CHECK(parse_input("#N/A").is_err() && parse_input("#N/A").err == Err::NA);
    CHECK(parse_input("(5)").num == -5);
    CHECK(parse_input("1e3").num == 1000);
    CHECK(parse_input("10k").is_str());
    CHECK(parse_input("R1-R5").is_str());
    CHECK(parse_input("2024-02-30").is_str());
    CHECK(parse_input("-0.5").num == -0.5);
    CHECK(parse_input("15-Jan-2024").num == 45306);
    CHECK(date_serial(1904, 1, 1, true) == 0 && date_serial(2024, 1, 15, true) == 45306 - 1462);
}

static void engine_suite() {
    // ---- recalculation, dependencies ----
    {
        Book b;
        b.set_input(0, 0, 0, "1");
        for (int r = 1; r < 200; ++r) b.set_input(0, r, 0, "=A" + std::to_string(r) + "+1");
        CHECK_EQ(b.display_text(0, 199, 0), std::string("200"));
        b.set_input(0, 0, 0, "100");
        CHECK_EQ(b.display_text(0, 199, 0), std::string("299"));
        b.set_input(0, 0, 1, "=SUM(A:A)");
        CHECK_EQ(b.display_text(0, 0, 1), std::to_string((100 + 299) * 200 / 2));
        b.set_input(0, 0, 0, "0");
        CHECK_EQ(b.display_text(0, 0, 1), std::to_string((0 + 199) * 200 / 2));
        // circular
        b.set_input(0, 0, 2, "=C2+1");
        b.set_input(0, 1, 2, "=C1+1");
        CHECK(b.sheets[0].value(0, 2).is_err() && b.sheets[0].value(0, 2).err == Err::Circ);
        CHECK(b.last_recalc().circular >= 2);
        b.set_input(0, 1, 2, "5");
        CHECK_EQ(b.display_text(0, 0, 2), std::string("6"));
        // reverse-order chain (exercises the explicit work stack)
        Book c;
        for (int r = 0; r < 5000; ++r) c.set_input(0, r, 0, r == 4999 ? "1" : "=A" + std::to_string(r + 2) + "*1");
        CHECK_EQ(c.display_text(0, 0, 0), std::string("1"));
        CHECK(c.dependents(0, 1, 0).size() == 1 && c.precedents(0, 0, 0).size() == 1);
    }
    // ---- spill ----
    {
        Book b;
        b.set_input(0, 0, 0, "=SEQUENCE(3,2)");
        CHECK_EQ(b.display_text(0, 2, 1), std::string("6"));
        CHECK(b.sheets[0].spill_anchor(2, 1) == std::make_pair(0, 0));
        b.set_input(0, 0, 3, "=SUM(A1#)");
        CHECK_EQ(b.display_text(0, 0, 3), std::string("21"));
        b.set_input(0, 1, 1, "x");
        CHECK(b.sheets[0].value(0, 0).is_err() && b.sheets[0].value(0, 0).err == Err::Spill);
        b.clear(0, Range(1, 1, 1, 1));
        CHECK_EQ(b.display_text(0, 2, 1), std::string("6"));
        b.set_input(0, 5, 0, "=UNIQUE({\"R1\";\"R2\";\"R1\"})");
        CHECK_EQ(b.display_text(0, 6, 0), std::string("R2"));
        CHECK(b.display_text(0, 7, 0).empty());
    }
    // ---- reference adjustment ----
    {
        Book b;
        b.set_input(0, 0, 0, "1"); b.set_input(0, 1, 0, "2"); b.set_input(0, 2, 0, "3");
        b.set_input(0, 4, 0, "=SUM(A1:A3)");
        b.set_input(0, 4, 1, "=$A$1+A2");
        b.add_sheet("S2");
        b.set_input(1, 0, 0, "=Sheet1!A3*2");
        b.insert_rows(0, 1, 2);   // above A2
        CHECK_EQ(b.input_text(0, 6, 0), std::string("=SUM(A1:A5)"));
        CHECK_EQ(b.input_text(0, 6, 1), std::string("=$A$1+A4"));
        CHECK_EQ(b.input_text(1, 0, 0), std::string("=Sheet1!A5*2"));
        CHECK_EQ(b.display_text(0, 6, 0), std::string("6"));
        b.delete_rows(0, 0, 1);   // delete row 1 (A1)
        CHECK_EQ(b.input_text(0, 5, 1), std::string("=#REF!+A3"));
        CHECK_EQ(b.input_text(0, 5, 0), std::string("=SUM(A1:A4)"));
        CHECK_EQ(b.display_text(0, 5, 1), std::string("#REF!"));
        b.undo();
        CHECK_EQ(b.input_text(0, 6, 1), std::string("=$A$1+A4"));
        CHECK_EQ(b.display_text(0, 6, 0), std::string("6"));
        b.redo();
        CHECK_EQ(b.input_text(0, 5, 0), std::string("=SUM(A1:A4)"));
        b.undo();
        b.insert_cols(0, 0, 1);
        CHECK_EQ(b.input_text(0, 6, 2), std::string("=$B$1+B4"));
        CHECK_EQ(b.input_text(1, 0, 0), std::string("=Sheet1!B5*2"));
        b.delete_cols(0, 1, 1);   // column B (the SUM itself moved there) -> refs into B die
        CHECK_EQ(b.input_text(0, 6, 1), std::string("=#REF!+#REF!"));
        b.undo();
        // range end deleted -> shrinks
        b.delete_rows(0, 4, 1);   // last row of SUM(B1:B5)
        CHECK_EQ(b.input_text(0, 5, 1), std::string("=SUM(B1:B4)"));
        // whole column refs unaffected by row ops
        Book c;
        c.set_input(0, 0, 1, "=SUM(A:A)");
        c.insert_rows(0, 0, 3);
        CHECK_EQ(c.input_text(0, 3, 1), std::string("=SUM(A:A)"));
        // defined names follow structural edits
        Book d;
        d.define_name({"Parts", -1, "Sheet1!$A$2:$A$4"});
        d.insert_rows(0, 0, 1);
        CHECK_EQ(d.names[0].formula, std::string("Sheet1!$A$3:$A$5"));
        // sheet rename keeps formula text in sync; delete -> #REF!
        b.set_input(1, 0, 1, "=Sheet1!B2*2");
        b.rename_sheet(0, "BOM List");
        CHECK_EQ(b.input_text(1, 0, 1), std::string("='BOM List'!B2*2"));
        b.remove_sheet(0);
        CHECK_EQ(b.input_text(0, 0, 1), std::string("=#REF!*2"));
        b.undo();
        CHECK_EQ(b.sheet_count(), 2);
        CHECK_EQ(b.input_text(1, 0, 1), std::string("='BOM List'!B2*2"));
    }
    // ---- fill, copy/paste, sort, filter ----
    {
        Book b;
        b.set_input(0, 0, 0, "1"); b.set_input(0, 1, 0, "3");
        b.set_input(0, 0, 1, "=A1*10");
        b.set_input(0, 0, 2, "R1");
        b.fill(0, Range(0, 0, 1, 0), Range(0, 0, 5, 0));
        CHECK_EQ(b.display_text(0, 5, 0), std::string("11"));
        b.fill(0, Range(0, 1, 0, 1), Range(0, 1, 5, 1));
        CHECK_EQ(b.input_text(0, 5, 1), std::string("=A6*10"));
        CHECK_EQ(b.display_text(0, 5, 1), std::string("110"));
        b.fill(0, Range(0, 2, 0, 2), Range(0, 2, 3, 2));
        CHECK_EQ(b.display_text(0, 3, 2), std::string("R4"));
        ClipBlock blk = b.copy(0, Range(0, 1, 1, 1));
        b.paste(0, 0, 4, blk);
        CHECK_EQ(b.input_text(0, 1, 4), std::string("=D2*10"));
        b.paste(0, 0, 5, blk, PasteValues);
        CHECK_EQ(b.input_text(0, 1, 5), std::string("30"));
        CHECK(b.undo() && b.input_text(0, 1, 5).empty());
        std::string tsv = b.copy_tsv(0, Range(0, 0, 1, 2));
        CHECK_EQ(tsv, std::string("1\t10\tR1\r\n3\t30\tR2\r\n"));
        ClipBlock t = clip_from_tsv("5\t=A1+1\n0805\t12%\n");
        b.paste(0, 10, 0, t);
        CHECK_EQ(b.input_text(0, 10, 1), std::string("=A1+1"));
        CHECK_EQ(b.display_text(0, 11, 0), std::string("0805"));
        CHECK_EQ(b.display_text(0, 11, 1), std::string("12%"));
        // html
        std::string html = cf_html_wrap(b.copy_html(0, Range(0, 0, 1, 2)));
        CHECK(html.rfind("Version:0.9", 0) == 0);
        size_t sf = (size_t)atol(html.c_str() + html.find("StartFragment:") + 14), ef = (size_t)atol(html.c_str() + html.find("EndFragment:") + 12);
        CHECK(html.substr(sf, 6) == "<table" && html.substr(ef - 9, 8) == "</table>");
        ClipBlock hb;
        CHECK(parse_html_table(cf_html_unwrap(html), hb));
        CHECK(hb.rows == 2 && hb.cols == 3 && hb.cells[5].v.str == "R2" && hb.cells[0].v.num == 1);
        CHECK(hb.formula_text[1] == "A1*10");
        ClipBlock xh;
        CHECK(parse_html_table("<style>.xl65{font-weight:700;color:#FF0000}</style><table><tr><td class=xl65>Ref</td><td style='background:#00ff00' colspan=2>Qty &amp; x</td></tr>"
                               "<tr><td>R1</td><td x:num=\"5\">5.0</td><td style=\"mso-number-format:'\\@'\">0805</td></tr></table>", xh));
        CHECK(xh.rows == 2 && xh.cols == 3);
        CHECK(xh.styles[xh.cells[0].style].font.bold && xh.styles[xh.cells[0].style].font.color == 0xFFFF0000);
        CHECK(xh.cells[1].v.str == "Qty & x" && xh.styles[xh.cells[1].style].fill.fg == 0xFF00FF00);
        CHECK(xh.cells[4].v.num == 5 && xh.cells[5].v.is_str());
        // multi-key natural sort
        Book s;
        s.set_inputs(0, 0, 0, {{"Ref", "Val"}, {"R10", "b"}, {"R2", "a"}, {"C1", "b"}, {"R1", "a"}, {"", "z"}});
        s.set_input(0, 1, 2, "=B2&\"!\"");
        s.sort(0, Range(0, 0, 5, 2), {{1, false}, {0, false}}, true);
        CHECK_EQ(s.display_text(0, 1, 0), std::string("R1"));
        CHECK_EQ(s.display_text(0, 2, 0), std::string("R2"));
        CHECK_EQ(s.display_text(0, 3, 0), std::string("C1"));
        CHECK_EQ(s.display_text(0, 4, 0), std::string("R10"));
        CHECK_EQ(s.display_text(0, 4, 2), std::string("b!"));
        s.sort(0, Range(0, 0, 5, 1), {{0, true}}, true);
        CHECK_EQ(s.display_text(0, 1, 0), std::string("R10"));
        CHECK_EQ(s.display_text(0, 5, 0), std::string(""));
        // autofilter
        s.set_autofilter(0, Range(0, 0, 5, 1));
        FilterColumn fc; fc.col = 1; fc.values = {"a"};
        s.set_filter_column(0, fc);
        int hidden = 0; for (int r = 1; r <= 5; ++r) hidden += s.sheets[0].row_hidden(r);
        CHECK(hidden == 3);
        auto fv = s.filter_values(0, 1);
        CHECK(fv.size() == 3 && fv[0] == "a");
        s.set_input(0, 7, 0, "=SUBTOTAL(103,A2:A6)");
        CHECK_EQ(s.display_text(0, 7, 0), std::string("2"));
        s.clear_filters(0);
        CHECK(!s.sheets[0].row_hidden(2));
        FilterColumn cf2; cf2.col = 0; cf2.use_custom = true; cf2.op1 = CmpOp::Equal; cf2.v1 = "R*";
        s.set_filter_column(0, cf2);
        hidden = 0; for (int r = 1; r <= 5; ++r) hidden += s.sheets[0].row_hidden(r);
        CHECK(hidden == 2);
    }
    // ---- styles, look, conditional formats, validation ----
    {
        Book b;
        b.set_inputs(0, 0, 0, {{"5"}, {"15"}, {"5"}, {"x"}, {"=1/0"}});
        b.apply_style(0, Range(0, 0, 4, 0), [](Style& s) { s.font.bold = true; s.border.bottom = {BorderStyle::Thin, 0xFF000000}; }, "Bold");
        CHECK(b.style_of(0, 2, 0).font.bold);
        Dxf red; red.fill_color = 0xFFFF0000; red.bold = true;
        b.dxfs.push_back(red);
        CondFormat cf; cf.ranges = {Range(0, 0, 4, 0)};
        CondRule r1; r1.type = CondRule::CellIs; r1.op = CmpOp::Greater; r1.f1 = "10"; r1.dxf = 0; r1.priority = 1;
        CondRule r2; r2.type = CondRule::Duplicate; r2.dxf = 0; r2.priority = 2;
        cf.rules = {r1, r2};
        b.add_cond_format(0, cf);
        CHECK(b.look(0, 1, 0).fill_color == 0xFFFF0000);
        CHECK(b.look(0, 0, 0).fill_color == 0xFFFF0000);   // duplicate 5
        CHECK(b.look(0, 4, 0).fill_color == 0);   // errors never match cellIs
        CondFormat cs; cs.ranges = {Range(0, 1, 2, 1)};
        b.set_inputs(0, 0, 1, {{"0"}, {"50"}, {"100"}});
        CondRule sc; sc.type = CondRule::ColorScale; sc.cfvo = {{CfValueObject::Min, "", 0xFFFFFFFF}, {CfValueObject::Max, "", 0xFF000000}};
        CondRule db; db.type = CondRule::DataBar; db.cfvo = {{CfValueObject::Min, ""}, {CfValueObject::Max, ""}}; db.priority = 2;
        cs.rules = {sc, db};
        b.add_cond_format(0, cs);
        CellLook mid = b.look(0, 1, 1);
        CHECK(mid.fill_color == 0xFF808080 || mid.fill_color == 0xFF7F7F7F);
        CHECK(std::fabs(mid.bar - 0.5f) < 1e-6);
        CHECK(b.look(0, 4, 0).align == HAlign::Center);
        CHECK(b.look(0, 0, 0).align == HAlign::Right && b.look(0, 3, 0).align == HAlign::Left);
        DataValidation dv; dv.type = DataValidation::List; dv.f1 = "\"Yes,No, Maybe\""; dv.ranges = {Range(0, 3, 9, 3)};
        b.add_validation(0, dv);
        auto items = b.list_items(0, 2, 3);
        CHECK(items.size() == 3 && items[2] == "Maybe");
        CHECK(b.look(0, 2, 3).dropdown);
        CHECK(b.validate(0, 2, 3, Value::text("no")));
        CHECK(!b.validate(0, 2, 3, Value::text("perhaps")));
        DataValidation dv2; dv2.type = DataValidation::Whole; dv2.op = CmpOp::Between; dv2.f1 = "1"; dv2.f2 = "10"; dv2.ranges = {Range(0, 4, 0, 4)};
        b.add_validation(0, dv2);
        CHECK(b.validate(0, 0, 4, Value::number(5)) && !b.validate(0, 0, 4, Value::number(5.5)) && !b.validate(0, 0, 4, Value::number(11)));
        DataValidation dv3; dv3.type = DataValidation::List; dv3.f1 = "$B$1:$B$3"; dv3.ranges = {Range(0, 5, 0, 5)};
        b.add_validation(0, dv3);
        CHECK(b.list_items(0, 0, 5).size() == 3);
        // typed input adopts a number format
        b.set_input(0, 6, 0, "3/15/2024");
        CHECK_EQ(b.display_text(0, 6, 0), std::string("3/15/2024"));
        CHECK_EQ(b.input_text(0, 6, 0), std::string("3/15/2024"));
        b.set_input(0, 7, 0, "25%");
        CHECK_EQ(b.input_text(0, 7, 0), std::string("25%"));
        b.set_number_format(0, Range(7, 0, 7, 0), "0.0%");
        CHECK_EQ(b.display_text(0, 7, 0), std::string("25.0%"));
        // batch undo
        b.begin_batch("Many");
        for (int i = 0; i < 10; ++i) b.set_input(0, 20 + i, 0, std::to_string(i));
        b.end_batch();
        CHECK_EQ(b.undo_label(), std::string("Many"));
        b.undo();
        CHECK(b.display_text(0, 25, 0).empty());
        // merges / freeze / widths
        b.merge(0, Range(10, 0, 11, 2));
        CHECK(b.sheets[0].merge_at(11, 1) != nullptr);
        b.set_freeze(0, 1, 2);
        b.set_col_width(0, 0, 0, 20);
        CHECK(b.sheets[0].col_width(0) == 20 && b.sheets[0].freeze_cols == 2);
        // functions catalog
        CHECK(function_catalog().size() >= 150);
        CHECK(find_function("_xlfn.XLOOKUP") && find_function("vlookup")->min_args == 3);
        auto lex = tokenize_formula("=SUM(A1:B2,\"x\")");
        CHECK(lex.size() >= 6 && lex[0].kind == LexToken::Function && lex[2].kind == LexToken::Ref);
        auto refs = formula_refs("=A1+Sheet1!B2:C3", b, 0, 0, 0);
        CHECK(refs.size() == 2 && refs[1].rg == Range(1, 1, 2, 2));
        CHECK_EQ(refdes_count("R1-R5,R7"), 6);
        // legacy bridge
        Workbook lw = to_legacy(b);
        CHECK(lw.sheets[0].at(1, 0) == "15");
        Book fb = from_legacy(lw);
        CHECK(fb.sheets[0].value(1, 0).num == 15);
    }
}

static std::string tmpdir() { return "build/tests"; }

static bool have(const char* cmd) { std::string c = std::string("command -v ") + cmd + " >/dev/null 2>&1"; return std::system(c.c_str()) == 0; }

static Book styled_book() {
    Book b;
    b.rename_sheet(0, "BOM");
    b.set_inputs(0, 0, 0, {{"Ref", "Part", "Qty", "Price", "Ext", "Date"},
                           {"R1-R5", "0805", "5", "0.1", "=C2*D2", "3/15/2024"},
                           {"C1", "x&y<z>\"", "2", "1.25", "=C3*D3", "2024-01-15"},
                           {"U1", "\xE6\x97\xA5\xE6\x9C\xAC", "1", "12", "=C4*D4", ""},
                           {"", "Total", "=SUM(C2:C4)", "", "=SUM(E2:E4)", ""}});
    b.set_input(0, 5, 0, "=REFCOUNT(A2:A4)");
    b.set_input(0, 6, 0, "=XLOOKUP(\"C1\",A2:A4,B2:B4)");
    b.set_input(0, 7, 0, "=SEQUENCE(2)");
    b.set_input(0, 10, 0, " lead space");
    b.set_input(0, 11, 0, "multi\nline");
    b.set_input(0, 12, 0, "TRUE");
    b.set_input(0, 13, 0, "=1/0");
    b.apply_style(0, Range(0, 0, 0, 5), [](Style& s) {
        s.font.bold = true; s.font.color = 0xFFFFFFFF; s.fill.pattern = FillPattern::Solid; s.fill.fg = 0xFF305496;
        s.halign = HAlign::Center; s.border.bottom = {BorderStyle::Medium, 0xFF000000};
    }, "Header");
    b.set_number_format(0, Range(1, 3, 4, 4), "\"$\"#,##0.00");
    b.apply_style(0, Range(1, 1, 3, 1), [](Style& s) { s.font.italic = true; s.font.name = "Arial"; s.font.size = 9; s.wrap = true; s.valign = VAlign::Top; }, "Fmt");
    b.apply_style(0, Range(4, 0, 4, 5), [](Style& s) { s.border.top = {BorderStyle::Double, 0xFFFF0000}; s.font.underline = Underline::Single; s.font.strike = true; s.indent = 2; }, "Total");
    b.set_col_width(0, 0, 0, 14.5);
    b.set_col_width(0, 1, 1, 30);
    b.hide_cols(0, 6, 6, true);
    b.set_row_height(0, 0, 0, 24);
    b.hide_rows(0, 9, 9, true);
    b.merge(0, Range(15, 0, 15, 3));
    b.set_input(0, 15, 0, "Merged title");
    b.set_freeze(0, 1, 1);
    b.set_tab_color(0, 0xFF00B050);
    b.set_autofilter(0, Range(0, 0, 3, 5));
    Dxf d; d.fill_color = 0xFFFFC7CE; d.font_color = 0xFF9C0006; d.bold = true;
    b.dxfs.push_back(d);
    CondFormat cf; cf.ranges = {Range(1, 2, 3, 2)};
    CondRule r; r.type = CondRule::CellIs; r.op = CmpOp::GreaterEqual; r.f1 = "2"; r.dxf = 0; r.priority = 1;
    CondRule r2; r2.type = CondRule::ColorScale; r2.priority = 2; r2.cfvo = {{CfValueObject::Min, "", 0xFFF8696B}, {CfValueObject::Percentile, "50", 0xFFFFEB84}, {CfValueObject::Max, "", 0xFF63BE7B}};
    cf.rules = {r, r2};
    b.add_cond_format(0, cf);
    CondFormat cf2; cf2.ranges = {Range(1, 0, 3, 0)};
    CondRule r3; r3.type = CondRule::ContainsText; r3.text = "R"; r3.dxf = 0; r3.priority = 3;
    cf2.rules = {r3};
    b.add_cond_format(0, cf2);
    DataValidation dv; dv.type = DataValidation::List; dv.f1 = "\"Yes,No\""; dv.ranges = {Range(1, 6, 3, 6)}; dv.prompt = "pick"; dv.error = "bad";
    b.add_validation(0, dv);
    DataValidation dv2; dv2.type = DataValidation::Whole; dv2.op = CmpOp::GreaterEqual; dv2.f1 = "0"; dv2.ranges = {Range(1, 2, 3, 2)};
    b.add_validation(0, dv2);
    b.define_name({"Qtys", -1, "BOM!$C$2:$C$4"});
    int s2 = b.add_sheet("Sheet 2");
    b.set_input(s2, 0, 0, "=SUM(BOM!C2:C4)");
    b.set_input(s2, 1, 0, "=SUM(Qtys)*2");
    b.set_input(s2, 2, 0, "=BOM!B3");
    return b;
}

static void compare_books(const Book& a, const Book& b, const char* tag) {
    int bad = 0;
    for (int si = 0; si < a.sheet_count() && si < b.sheet_count(); ++si) {
        const Worksheet& x = a.sheets[si];
        const Worksheet& y = b.sheets[si];
        if (x.name != y.name) { ++bad; std::printf("FAIL %s sheet name '%s' vs '%s'\n", tag, x.name.c_str(), y.name.c_str()); }
        int nr = std::max(x.nrows(), y.nrows()), nc = std::max(x.ncols(), y.ncols());
        for (int r = 0; r < nr; ++r)
            for (int c = 0; c < nc; ++c) {
                std::string ta = a.display_text(si, r, c), tb = b.display_text(si, r, c);
                if (ta != tb) { if (bad++ < 10) std::printf("FAIL %s %s!%s display '%s' vs '%s'\n", tag, x.name.c_str(), cell_name(r, c).c_str(), ta.c_str(), tb.c_str()); }
                std::string fa = a.input_text(si, r, c), fb = b.input_text(si, r, c);
                if (!fa.empty() && fa[0] == '=' && fa != fb) { if (bad++ < 10) std::printf("FAIL %s %s formula '%s' vs '%s'\n", tag, cell_name(r, c).c_str(), fa.c_str(), fb.c_str()); }
                const Style& sa = a.style_of(si, r, c);
                const Style& sb = b.style_of(si, r, c);
                if (!(sa.font.bold == sb.font.bold && sa.font.italic == sb.font.italic && sa.fill.fg == sb.fill.fg && sa.border.bottom.style == sb.border.bottom.style &&
                      sa.border.top.style == sb.border.top.style && sa.halign == sb.halign && sa.wrap == sb.wrap && sa.font.color == sb.font.color && sa.numfmt == sb.numfmt))
                    if (bad++ < 10) std::printf("FAIL %s %s style differs (bold %d/%d fill %08X/%08X fmt '%s'/'%s')\n", tag, cell_name(r, c).c_str(), sa.font.bold, sb.font.bold, sa.fill.fg, sb.fill.fg, sa.numfmt.c_str(), sb.numfmt.c_str());
            }
        if (x.merges.size() != y.merges.size()) { ++bad; std::printf("FAIL %s merges\n", tag); }
        if (x.freeze_rows != y.freeze_rows || x.freeze_cols != y.freeze_cols) { ++bad; std::printf("FAIL %s freeze %d,%d vs %d,%d\n", tag, x.freeze_rows, x.freeze_cols, y.freeze_rows, y.freeze_cols); }
        if (std::fabs(x.col_width(1) - y.col_width(1)) > 0.2) { ++bad; std::printf("FAIL %s col width %g vs %g\n", tag, x.col_width(1), y.col_width(1)); }
        if (x.col_hidden(6) != y.col_hidden(6) || x.row_hidden(9) != y.row_hidden(9)) { ++bad; std::printf("FAIL %s hidden rows/cols\n", tag); }
        if (x.cond.size() != y.cond.size() || x.validations.size() != y.validations.size()) { ++bad; std::printf("FAIL %s cf %zu/%zu dv %zu/%zu\n", tag, x.cond.size(), y.cond.size(), x.validations.size(), y.validations.size()); }
        if (x.autofilter.active() != y.autofilter.active()) { ++bad; std::printf("FAIL %s autofilter\n", tag); }
        if (x.tab_color != y.tab_color) { ++bad; std::printf("FAIL %s tab colour %08X vs %08X\n", tag, x.tab_color, y.tab_color); }
    }
    if (a.sheet_count() != b.sheet_count()) { ++bad; std::printf("FAIL %s sheet count\n", tag); }
    if (a.names.size() != b.names.size()) { ++bad; std::printf("FAIL %s names %zu vs %zu\n", tag, a.names.size(), b.names.size()); }
    if (bad) g_fail += bad; else ++g_ok;
}

static void io_suite() {
    // ---- DEFLATE ----
    {
        std::mt19937 rng(7);
        std::vector<std::vector<uint8_t>> inputs;
        inputs.push_back({});
        inputs.push_back({'a'});
        std::string text;
        for (int i = 0; i < 20000; ++i) text += "<c r=\"A" + std::to_string(i) + "\"><v>" + std::to_string(rng() % 1000) + "</v></c>";
        inputs.push_back(std::vector<uint8_t>(text.begin(), text.end()));
        std::vector<uint8_t> rnd(100000); for (auto& x : rnd) x = (uint8_t)rng(); inputs.push_back(rnd);
        std::vector<uint8_t> runs(300000, 'z'); inputs.push_back(runs);
        std::vector<uint8_t> mixed; for (int i = 0; i < 50000; ++i) mixed.push_back((uint8_t)(i % 7 == 0 ? rng() : 'a' + i % 3)); inputs.push_back(mixed);
        for (int lvl : {0, 1, 6, 9}) {
            for (auto& in : inputs) {
                auto z = deflate_raw(in.data(), in.size(), lvl);
                std::vector<uint8_t> out; std::string err;
                bool ok = inflate_raw(z.data(), z.size(), out, in.size(), err);
                if (ok && out == in) ++g_ok; else { ++g_fail; std::printf("FAIL deflate level %d size %zu: %s\n", lvl, in.size(), err.c_str()); }
            }
        }
        auto zt = deflate_raw(inputs[2].data(), inputs[2].size(), 6);
        std::printf("deflate: xml %zu -> %zu bytes (%.1f%%)\n", inputs[2].size(), zt.size(), 100.0 * zt.size() / inputs[2].size());
        CHECK(zt.size() < inputs[2].size() / 4);
        auto zr = deflate_raw(rnd.data(), rnd.size(), 6);
        CHECK(zr.size() <= rnd.size() + rnd.size() / 1000 + 16);   // stored fallback
        // cross-check with python zlib
        if (have("python3")) {
            auto zz = zlib_compress(inputs[2].data(), inputs[2].size(), 9);
            std::string e;
            write_file((tmpdir() + "/deflate.zlib").c_str(), zz.data(), zz.size(), e);
            write_file((tmpdir() + "/deflate.raw").c_str(), inputs[2].data(), inputs[2].size(), e);
            int rc = std::system(("python3 -c \"import zlib,sys; a=open('" + tmpdir() + "/deflate.zlib','rb').read(); b=open('" + tmpdir() + "/deflate.raw','rb').read(); sys.exit(0 if zlib.decompress(a)==b else 1)\"").c_str());
            CHECK(rc == 0);
            rc = std::system(("python3 -c \"import zlib; open('" + tmpdir() + "/py.zlib','wb').write(zlib.compress(open('" + tmpdir() + "/deflate.raw','rb').read(),9))\"").c_str());
            std::vector<uint8_t> pz, back; read_file((tmpdir() + "/py.zlib").c_str(), pz, e);
            CHECK(rc == 0 && zlib_uncompress(pz.data(), pz.size(), back, 0, e) && back == inputs[2]);
            std::printf("deflate: ours(9) %zu bytes, python zlib(9) %zu bytes\n", zz.size(), pz.size());
        }
    }
    // ---- XLSX round trip with styles ----
    Book src = styled_book();
    {
        auto bytes = io::xlsx_write(src);
        std::string e;
        std::string path = tmpdir() + "/styled.xlsx";
        write_file(path.c_str(), bytes.data(), bytes.size(), e);
        Book rb; std::string err;
        CHECK(io::xlsx_read(bytes, rb, err));
        compare_books(src, rb, "xlsx");
        CHECK(rb.sheets[0].spills.size() == 1);
        CHECK(rb.dxfs.size() == 1 && rb.sheets[0].cond[0].rules[1].cfvo.size() == 3);
        CHECK(rb.sheets[0].validations[0].f1 == "\"Yes,No\"" && rb.sheets[0].validations[0].prompt == "pick");
        CHECK(rb.names.size() == 1 && rb.names[0].formula == "BOM!$C$2:$C$4");
        // second generation identical bytes
        auto bytes2 = io::xlsx_write(rb);
        Book rb2; CHECK(io::xlsx_read(bytes2, rb2, err));
        compare_books(rb, rb2, "xlsx gen2");
        if (have("python3") && std::system("python3 -c 'import openpyxl' 2>/dev/null") == 0) {
            const char* py =
                "import openpyxl,sys\n"
                "wb=openpyxl.load_workbook(sys.argv[1])\n"
                "ws=wb['BOM']\n"
                "e=[]\n"
                "def chk(c,m):\n"
                "  (None if c else e.append(m))\n"
                "chk(ws['A1'].value=='Ref','A1')\n"
                "chk(ws['A1'].font.b and ws['A1'].fill.fgColor.rgb=='FF305496','A1 style')\n"
                "chk(ws['A1'].alignment.horizontal=='center','align')\n"
                "chk(ws['A1'].border.bottom.style=='medium','border')\n"
                "chk(ws['B2'].value=='0805' and ws['C2'].value==5,'values')\n"
                "chk(ws['E2'].value=='=C2*D2','formula')\n"
                "chk(ws['D2'].number_format=='\"$\"#,##0.00','numfmt %r'%ws['D2'].number_format)\n"
                "chk(ws['F2'].is_date,'date')\n"
                "chk(ws['B3'].value=='x&y<z>\"','escape')\n"
                "chk(ws['B2'].font.i and ws['B2'].font.name=='Arial' and ws['B2'].alignment.wrap_text,'font')\n"
                "chk(ws.freeze_panes=='B2','freeze %r'%ws.freeze_panes)\n"
                "chk(abs(ws.column_dimensions['B'].width-30)<0.01,'width')\n"
                "chk(ws.column_dimensions['G'].hidden,'hidden col')\n"
                "chk(ws.row_dimensions[10].hidden,'hidden row')\n"
                "chk(ws.row_dimensions[1].height==24,'row height')\n"
                "chk('A16:D16' in [str(r) for r in ws.merged_cells.ranges],'merge')\n"
                "chk(ws.auto_filter.ref=='A1:F4','autofilter')\n"
                "chk(len(ws.conditional_formatting)==2,'cf')\n"
                "chk(len(ws.data_validations.dataValidation)==2,'dv')\n"
                "chk(ws.sheet_properties.tabColor.rgb=='FF00B050','tab')\n"
                "chk(ws['A11'].value==' lead space' and ws['A12'].value=='multi\\nline','ws')\n"
                "chk(ws['A13'].value==True,'bool')\n"
                "chk('Qtys' in wb.defined_names,'name')\n"
                "chk(wb['Sheet 2']['A1'].value=='=SUM(BOM!C2:C4)','xsheet')\n"
                "chk(ws['A5'].border.top.style=='double' and ws['A5'].font.u=='single' and ws['A5'].font.strike,'total')\n"
                "w2=openpyxl.load_workbook(sys.argv[1],data_only=True)['BOM']\n"
                "chk(w2['E5'].value==15,'cached %r'%w2['E5'].value)\n"
                "chk(w2['A6'].value==7,'refcount')\n"
                "print('openpyxl:', 'OK' if not e else e)\n"
                "sys.exit(1 if e else 0)\n";
            std::string e2;
            std::string pyf = tmpdir() + "/check_xlsx.py";
            write_file(pyf.c_str(), py, strlen(py), e2);
            int rc = std::system(("python3 " + pyf + " " + path).c_str());
            CHECK(rc == 0);
        } else std::printf("SKIP openpyxl check\n");
        if (have("soffice")) {
            std::string cmd = "cd " + tmpdir() + " && rm -rf lo_x && mkdir lo_x && timeout 180 soffice --headless --convert-to csv:\"Text - txt - csv (StarCalc)\":44,34,76,1,,0,false,true,false,false,false,-1 --outdir lo_x styled.xlsx >/dev/null 2>&1";
            int rc = std::system(cmd.c_str());
            std::vector<uint8_t> csv; std::string e3;
            bool ok = rc == 0 && read_file((tmpdir() + "/lo_x/styled-BOM.csv").c_str(), csv, e3);
            if (!ok) ok = read_file((tmpdir() + "/lo_x/styled.csv").c_str(), csv, e3);
            std::string s(csv.begin(), csv.end());
            CHECK(ok && s.find("Ref,Part,Qty") != std::string::npos && (s.find("$15.00") != std::string::npos || s.find(",15,") != std::string::npos));
            std::printf("LibreOffice xlsx->csv: %s\n", ok ? "converted" : "FAILED");
        } else std::printf("SKIP LibreOffice check\n");
    }
    // ---- openpyxl-authored file: formulas without cached values, alpha-00 colours ----
    if (have("python3") && std::system("python3 -c 'import openpyxl' 2>/dev/null") == 0) {
        std::string path = tmpdir() + "/openpyxl_formulas.xlsx";
        std::string cmd = "python3 -c \"import openpyxl; from openpyxl.styles import PatternFill, Font; wb=openpyxl.Workbook(); ws=wb.active; ws['A1']=2; ws['A2']='=A1*3'; "
                          "ws['A1'].fill=PatternFill('solid', fgColor='00FF0000'); ws['A1'].font=Font(color='000000FF'); ws.sheet_properties.tabColor='0000FF00'; wb.save('" + path + "')\"";
        if (std::system(cmd.c_str()) == 0) {
            std::vector<uint8_t> d; std::string e, err;
            read_file(path.c_str(), d, e);
            Book ob;
            CHECK(io::xlsx_read(d, ob, err));
            CHECK_EQ(ob.display_text(0, 1, 0), std::string("6"));
            CHECK(ob.style_of(0, 0, 0).fill.fg == 0xFFFF0000 && ob.style_of(0, 0, 0).font.color == 0xFF0000FF);
            CHECK(ob.sheets[0].tab_color == 0xFF00FF00);
        }
    }
    // ---- ODS round trip ----
    {
        auto bytes = io::ods_write(src);
        std::string e, path = tmpdir() + "/styled.ods";
        write_file(path.c_str(), bytes.data(), bytes.size(), e);
        CHECK(io::is_ods_bytes(bytes));
        Book rb; std::string err;
        CHECK(io::ods_read(bytes, rb, err));
        if (!err.empty()) std::printf("ods err: %s\n", err.c_str());
        compare_books(src, rb, "ods");
        CHECK_EQ(io::a1_to_of("SUM(A1:B2,'Sheet 2'!C3)"), std::string("of:=SUM([.A1:.B2];[$'Sheet 2'.C3])"));
        CHECK_EQ(io::of_to_a1("of:=SUM([.A1:.B2];[$'Sheet 2'.C3])"), std::string("SUM(A1:B2,'Sheet 2'!C3)"));
        CHECK_EQ(io::of_to_a1("of:=SUM({1;2|3;4})"), std::string("SUM({1,2;3,4})"));
        CHECK_EQ(io::a1_to_of("SUM({1,2;3,4})"), std::string("of:=SUM({1;2|3;4})"));
        if (have("soffice")) {
            std::string cmd = "cd " + tmpdir() + " && rm -rf lo_o && mkdir lo_o && timeout 180 soffice --headless --convert-to xlsx --outdir lo_o styled.ods >/dev/null 2>&1";
            int rc = std::system(cmd.c_str());
            std::vector<uint8_t> x; std::string e3;
            bool ok = rc == 0 && read_file((tmpdir() + "/lo_o/styled.xlsx").c_str(), x, e3);
            Book lb; std::string err2;
            ok = ok && io::xlsx_read(x, lb, err2);
            CHECK(ok);
            if (ok) {
                CHECK_EQ(lb.display_text(0, 4, 4), std::string("$15.00"));
                CHECK_EQ(lb.input_text(0, 1, 4), std::string("=C2*D2"));
                CHECK(lb.style_of(0, 0, 0).font.bold && lb.style_of(0, 0, 0).fill.fg == 0xFF305496);
                CHECK(lb.sheets[0].merges.size() == 1);   // headless soffice writes no view settings (freeze)
                CHECK_EQ(lb.display_text(1, 1, 0), std::string("16"));
                std::printf("LibreOffice ods->xlsx: ok (%d sheets)\n", lb.sheet_count());
            }
            // and LibreOffice's own ODS -> our reader
            cmd = "cd " + tmpdir() + " && rm -rf lo_r && mkdir lo_r && timeout 180 soffice --headless --convert-to ods --outdir lo_r styled.xlsx >/dev/null 2>&1";
            rc = std::system(cmd.c_str());
            std::vector<uint8_t> od;
            if (rc == 0 && read_file((tmpdir() + "/lo_r/styled.ods").c_str(), od, e3)) {
                Book ob; std::string err3;
                CHECK(io::ods_read(od, ob, err3));
                CHECK_EQ(ob.display_text(0, 4, 4), std::string("$15.00"));
                CHECK_EQ(ob.input_text(0, 4, 2), std::string("=SUM(C2:C4)"));
                CHECK(ob.style_of(0, 0, 0).font.bold && ob.sheets[0].merges.size() == 1);
                CHECK(ob.sheets[0].validations.size() >= 1 && ob.sheets[0].cond.size() >= 1);
                CHECK_EQ(ob.display_text(0, 1, 5), std::string("3/15/2024"));
            } else { ++g_fail; std::printf("FAIL LibreOffice xlsx->ods\n"); }
        }
    }
    // ---- CSV ----
    {
        Book b;
        std::string text = "Ref;Qty;Desc\r\nR1;5;\"a;b\"\r\nC2;0805;\xE9t\xE9\r\n";
        std::vector<uint8_t> d(text.begin(), text.end());
        io::CsvOptions o; o.sep = 0; o.encoding = io::TextEncoding::Windows1252;
        std::string err;
        CHECK(io::csv_read(d, b, 0, o, err));
        CHECK(b.sheets[0].value(1, 1).num == 5 && b.display_text(0, 2, 1) == "0805" && b.display_text(0, 1, 2) == "a;b");
        CHECK_EQ(b.display_text(0, 2, 2), std::string("\xC3\xA9t\xC3\xA9"));
        io::CsvOptions w; w.sep = ','; w.encoding = io::TextEncoding::Utf16LE;
        auto out = io::csv_write(b, 0, w);
        CHECK(out.size() > 4 && out[0] == 0xFF && out[1] == 0xFE);
        CHECK_EQ(io::decode_text_bytes(out, io::TextEncoding::Auto), std::string("Ref,Qty,Desc\r\nR1,5,a;b\r\nC2,0805,\xC3\xA9t\xC3\xA9\r\n"));
    }
}

static void perf_suite() {
    Book b;
    b.auto_calc = false;
    const int N = 100000;
    for (int r = 0; r < N; ++r) {
        Worksheet& ws = b.sheets[0];
        ws.at(r, 0).v = Value::number(r % 97);
        ws.at(r, 1).v = Value::number((r % 13) * 0.5);
        ws.at(r, 3).v = Value::text(r % 3 ? "R" : "C");
        std::string rr = std::to_string(r + 1);
        b.set_formula_raw(0, r, 2, "A" + rr + "*B" + rr + "+IF(D" + rr + "=\"R\",1,0)");
        if (r) b.set_formula_raw(0, r, 4, "E" + std::to_string(r) + "+C" + rr);
        else b.set_formula_raw(0, r, 4, "C1");
    }
    b.set_formula_raw(0, 0, 5, "SUM(C:C)");
    b.set_formula_raw(0, 1, 5, "SUMIF(D:D,\"R\",C:C)");
    b.set_formula_raw(0, 2, 5, "VLOOKUP(96,A1:B100,2,FALSE)");
    auto t0 = std::chrono::steady_clock::now();
    RecalcStats st = b.recalc_all();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("perf: full recalc of %d formulas in %.0f ms\n", st.evaluated, ms);
    CHECK(st.evaluated == 2 * N + 3);
    CHECK(ms < 1000);
    double sum = 0, run = 0;
    for (int r = 0; r < N; ++r) { double c = (r % 97) * ((r % 13) * 0.5) + (r % 3 ? 1 : 0); sum += c; run += c; }
    CHECK(std::fabs(b.sheets[0].value(0, 5).num - sum) < 1e-6 * sum);
    CHECK(std::fabs(b.sheets[0].value(N - 1, 4).num - run) < 1e-6 * run);
    b.auto_calc = true;
    t0 = std::chrono::steady_clock::now();
    b.set_value(0, 50000, 0, Value::number(1000));
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("perf: incremental edit -> %d cells recalculated in %.0f ms\n", b.last_recalc().evaluated, ms);
    CHECK(b.last_recalc().evaluated < N);   // only the dependents of A50001
    CHECK(ms < 1000);
    t0 = std::chrono::steady_clock::now();
    auto bytes = io::xlsx_write(b);
    double wms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    Book rb; std::string err;
    t0 = std::chrono::steady_clock::now();
    CHECK(io::xlsx_read(bytes, rb, err));
    double rms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("perf: xlsx %d cells: write %.0f ms (%zu KB), read %.0f ms\n", 5 * N, wms, bytes.size() / 1024, rms);
    CHECK(rb.display_text(0, N - 1, 4) == b.display_text(0, N - 1, 4));
}

int main() {
    formula_suite();
    format_suite();
    engine_suite();
    io_suite();
    perf_suite();
    std::printf("test_sheet_engine: %d ok, %d failed\n", g_ok, g_fail);
    return g_fail ? 1 : 0;
}
