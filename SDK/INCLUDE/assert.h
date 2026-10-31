/*
 * ASSERT.H - assert()
 *
 * Not guarded against a second inclusion, on purpose: C lets NDEBUG
 * change between one #include <assert.h> and the next.
 */
#include <_pmdefs.h>

#undef assert
#ifdef NDEBUG
#define assert( e )     ((void)0)
#else
_PM_BEGIN_C
_PM_NORETURN void __pm_assert( const char *expr, const char *file, int line );
_PM_END_C
#define assert( e )     ((e) ? (void)0 : __pm_assert( #e, __FILE__, __LINE__ ))
#endif

#if !defined( static_assert ) && !defined( __cplusplus ) && defined( __STDC_VERSION__ ) && __STDC_VERSION__ >= 201112L
#define static_assert   _Static_assert
#endif
