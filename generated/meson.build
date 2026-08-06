axio_generated_config_header = custom_target(
  'axio-config-generated-header',
  input: axio_config_input,
  output: 'axio_config_generated.h',
  command: [axio_configure, 'generate', '@INPUT@', '@OUTPUT@'],
)
