#pragma  once

class VtableIndexFinder
{
    typedef int (VtableIndexFinder::*method_pointer)();
public:
    template<typename _MethodPtr>
    static int getIndexOf(_MethodPtr ptr) {
        return (reinterpret_cast<VtableIndexFinder*>(&fake_vtable_ptr)->**((VtableIndexFinder::method_pointer*)(&ptr)))();
    }
protected:
    int method0() { return 0; }
    int method1() { return 1; }
    int method2() { return 2; }
    int method3() { return 3; }
    int method4() { return 4; }
    int method5() { return 5; }
    int method6() { return 6; }
    int method7() { return 7; }
    int method8() { return 8; }
    int method9() { return 9; }
    int method10() { return 10; }
    int method11() { return 11; }
    int method12() { return 12; }
    int method13() { return 13; }
    int method14() { return 14; }
    int method15() { return 15; }
    int method16() { return 16; }
    int method17() { return 17; }
    int method18() { return 18; }
    int method19() { return 19; }

    int method20() { return 20; }
    int method21() { return 21; }
    int method22() { return 22; }
    int method23() { return 23; }
    int method24() { return 24; }
    int method25() { return 25; }
    int method26() { return 26; }
    int method27() { return 27; }
    int method28() { return 28; }
    int method29() { return 29; }

    int method30() { return 30; }
    int method31() { return 31; }
    int method32() { return 32; }
    int method33() { return 33; }
    int method34() { return 34; }
    int method35() { return 35; }
    int method36() { return 36; }
    int method37() { return 37; }
    int method38() { return 38; }
    int method39() { return 39; }

    int method40() { return 40; }
    int method41() { return 41; }
    int method42() { return 42; }
    int method43() { return 43; }
    int method44() { return 44; }
    int method45() { return 45; }
    int method46() { return 46; }
    int method47() { return 47; }
    int method48() { return 48; }
    int method49() { return 49; }

    int method50() { return 50; }
    int method51() { return 51; }
    int method52() { return 52; }
    int method53() { return 53; }
    int method54() { return 54; }
    int method55() { return 55; }
    int method56() { return 56; }
    int method57() { return 57; }
    int method58() { return 58; }
    int method59() { return 59; }

    int method60() { return 60; }
    int method61() { return 61; }
    int method62() { return 62; }
    int method63() { return 63; }
    int method64() { return 64; }
    int method65() { return 65; }
    int method66() { return 66; }
    int method67() { return 67; }
    int method68() { return 68; }
    int method69() { return 69; }
    int method70() { return 70; }
    int method71() { return 71; }
    int method72() { return 72; }
    int method73() { return 73; }
    int method74() { return 74; }
    int method75() { return 75; }
    int method76() { return 76; }
    int method77() { return 77; }
    int method78() { return 78; }
    int method79() { return 79; }
    int method80() { return 80; }
    int method81() { return 81; }
    int method82() { return 82; }
    int method83() { return 83; }
    int method84() { return 84; }
    int method85() { return 85; }
    int method86() { return 86; }
    int method87() { return 87; }
    int method88() { return 88; }
    int method89() { return 89; }
    int method90() { return 90; }
    int method91() { return 91; }
    int method92() { return 92; }
    int method93() { return 93; }
    int method94() { return 94; }
    int method95() { return 95; }
    int method96() { return 96; }
    int method97() { return 97; }
    int method98() { return 98; }
    int method99() { return 99; }

protected:
    typedef method_pointer fake_vtable_t [100];
    static fake_vtable_t   fake_vtable;
    static void*                  fake_vtable_ptr;
};