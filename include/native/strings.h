#ifndef OBY_STRINGS_H
#define OBY_STRINGS_H

/* Register the string natives (to_str, str_concat). Used by codegen to build
   interpolated strings, so they are not declared in the prelude. */
void strings_register_natives(void);

#endif // OBY_STRINGS_H
