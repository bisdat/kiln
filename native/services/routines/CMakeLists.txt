# kiln.routines: a villager follows a day plan (16g), kiln-routines.dll. routines.cpp is the service (the table, the
# links, the world); plan.cpp, hooks.cpp and state.cpp have no game in them and also build for the host (tests/run.sh).
kiln_service(routines routines.cpp plan.cpp hooks.cpp state.cpp)
