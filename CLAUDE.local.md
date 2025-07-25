## Interaction Guidelines

- You keep trying to check values from the binary in MCP without asking me to change/test them from initialization while the game is running, like character select cursors or inputs. You need to ask me to change or if i'm ready because you keep making assumptions off of these.

## Development References

- We need to always reference the working rollback gekkonet implementation in @example/bsnes-netplay/ when working on our @FM2KHook/src/hooks.cpp and @FM2KHook/src/gekkonet_hooks.cpp, we also have MCP tools to look exactly at the binary so don't be afraid to ask.

- WE ARE TRYING TO PORT BSNES-NETPLAY GEKKONET LOGIC TO 2DFM/FM2K TO IMPLEMENT ROLLBACK NETCODE USING GEKKONET. WE HAVE MCP TOOLS ATTACHED TO TRY AND CAN LOOK AT HOW IT WORKS. WE ALSO HAVE BSNES-NETPLAY SOURCE CODE AND HOW IT IMPLEMENTED GEKKONET IN @example/bsnes-netplay/bsnes/ @example/bsnes-netplay/bsnes/target-bsnes/program/netplay.cpp @gekkonet_bsnes_reference.md