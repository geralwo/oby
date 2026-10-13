#ifndef OBY_NATIVE_JSON_H
#define OBY_NATIVE_JSON_H

/* Register the `json_*` natives, backed by the vendored json.h
   (vendor/README.md). See `src/native/json.c`. */
void json_register_natives(void);

#endif // OBY_NATIVE_JSON_H
