#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/backend/emitter.h"
#include "../src/frontend/decoder.h"

#define BASE 0x80004000u

int main(void) {
    FILE* out = tmpfile();
    if (!out) {
        perror("tmpfile");
        return 1;
    }

    const PPCInst lfs = ppc_decode(0xC0240000u, BASE);
    if (lfs.op != PPC_OP_LFS) {
        fprintf(stderr, "LFS fixture decoded incorrectly\n");
        fclose(out);
        return 1;
    }

    emit_header(out);
    emit_function(out, &lfs, 1, BASE);
    emit_footer(out);
    fflush(out);

    if (fseek(out, 0, SEEK_END) != 0) {
        fclose(out);
        return 1;
    }
    const long size = ftell(out);
    if (size < 0 || fseek(out, 0, SEEK_SET) != 0) {
        fclose(out);
        return 1;
    }

    char* generated = calloc((size_t)size + 1, 1);
    if (!generated || fread(generated, 1, (size_t)size, out) != (size_t)size) {
        free(generated);
        fclose(out);
        return 1;
    }
    fclose(out);

    const char* expected =
        "if ((ctx->msr & PPC_MSR_FP) == 0 && "
        "!ppc_fp_available(ctx, 0x80004000u)) return;";
    if (!strstr(generated, expected)) {
        fprintf(stderr, "FPU codegen lost its local MSR[FP] fast path:\n%s\n", generated);
        free(generated);
        return 1;
    }

    free(generated);
    return 0;
}
