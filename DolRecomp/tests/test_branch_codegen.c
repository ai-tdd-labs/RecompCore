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

    const PPCInst blrl = ppc_decode(0x4E800021u, BASE);
    if (blrl.op != PPC_OP_BCLR || !blrl.lk) {
        fprintf(stderr, "BLRL fixture decoded incorrectly\n");
        fclose(out);
        return 1;
    }

    emit_header(out);
    emit_function(out, &blrl, 1, BASE);
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

    const char* capture = strstr(generated, "u32 branch_target = ctx->lr & ~3u;");
    const char* link = strstr(generated, "ctx->lr = 0x80004004u;");
    const char* branch = strstr(generated, "ctx->pc = branch_target;");
    const int valid = capture && link && branch && capture < link && link < branch;
    if (!valid) {
        fprintf(stderr, "BLRL must capture old LR before writing the link address:\n%s\n", generated);
        free(generated);
        return 1;
    }

    free(generated);
    return 0;
}
