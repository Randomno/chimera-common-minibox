# System test: build a gcc guest .wbx and run it through the host end-to-end.
# Reached only when the guest toolchain is present (guarded in ../meson.build).
# Uses the shared waterbox_guest_* variables from source/guest/meson.build.

guest_wbx = custom_target('guest.wbx',
  input: 'guest.c',
  output: 'guest.wbx',
  command: [
    waterbox_musl_gcc, waterbox_guest_cflags, waterbox_guest_ldflags,
    '-std=c11', '-o', '@OUTPUT@', '@INPUT@', waterbox_emulibc_obj, '-lgcc',
  ],
)

# run_guest comes from ../runner (built for every target, cross included)
test('conformance-guest', run_guest,
  args: [guest_wbx],
  suite: 'system',
  timeout: 120,
)

# Dropped-%fs repair: guest toolchain only (Linux), like the guest itself.
test('conformance-fs-repair', run_guest,
  args: ['--fs-repair-child', guest_wbx],
  suite: 'system',
  timeout: 120,
)
