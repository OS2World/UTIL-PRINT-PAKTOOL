/* PAKT.CMD  Build PAKTOOL executable */
ARG dr_type

SELECT
    WHEN dr_type == '1' | LEFT( dr_type, 1 ) == 'P' THEN DO
        level = 1
        exename = 'ppaktool'
    END
    WHEN dr_type == '2' | LEFT( dr_type, 1 ) == 'E' THEN DO
        level = 2
        exename = 'epaktool'
    END
    WHEN dr_type == '3' | LEFT( dr_type, 1 ) == 'I' THEN DO
        level = 3
        exename = 'ipaktool'
    END
    OTHERWISE DO
        SAY 'To build PAKTOOL executable, use syntax:'
        SAY '  PAKT <opt>'
        SAY 'where <opt> indicates which driver PAKTOOL should be compatible with:'
        SAY '1  PSPRINT'
        SAY '2  ECUPS or DDK mainline PSCRIPT driver'
        SAY '3  IBM post-DDK PSCRIPT driver (e.g. 30.822 or 30.827)'
        RETURN 0
    END
END

'icc /Sp1 /Ss /DPSDRIVER='level' /Fe'exename' paktool.c'

RETURN rc
