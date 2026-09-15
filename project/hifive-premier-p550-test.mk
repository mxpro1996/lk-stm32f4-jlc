# main project for hifive premier p550 test project
MODULES += \
	app/shell

include project/virtual/test.mk
include project/target/hifive-premier-p550.mk
