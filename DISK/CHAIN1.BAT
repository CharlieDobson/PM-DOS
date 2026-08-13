@ECHO OFF
ECHO CHAIN1 running, about to chain to CHAIN2 without CALL
CHAIN2
ECHO CHAIN1: THIS MUST NOT PRINT - chaining does not return
