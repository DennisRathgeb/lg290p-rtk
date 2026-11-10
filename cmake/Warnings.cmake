add_library(lgrtk_warnings INTERFACE)
target_compile_options(lgrtk_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
  -Wno-sign-conversion $<$<BOOL:${LGRTK_WERROR}>:-Werror>)
