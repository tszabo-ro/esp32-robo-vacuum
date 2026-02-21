# AI-Assisted Development Guidelines

## Project Overview
ESP32-C3 SuperMini based controller for Neato D5 vacuum, integrated with Home Assistant via MQTT, built on FreeRTOS.

**Tech Stack:**
- C++20
- ESP-IDF (FreeRTOS)
- CMake build system
- MQTT protocol
- Docker-based toolchain

## Working Principles

### 1. Iteration Philosophy
- **Small, Atomic Changes**: Each iteration addresses one focused task
- **Test Early, Test Often**: Validate after every meaningful change
- **Fail Fast**: Catch issues immediately, not after multiple changes
- **Review Before Proceed**: Explicit approval required before moving to next task

### 2. Change Size Guidelines
**Target:** 50-150 lines of code per iteration (excluding boilerplate)

**One iteration = ONE of:**
- Single class/module implementation
- One feature addition
- One bug fix
- Configuration/setup change
- Documentation update

**Never in one iteration:**
- Multiple unrelated features
- Large refactoring + new features
- Cross-cutting changes without explicit agreement

### 3. Review Checkpoints
After each change:
1. **AI presents**: What changed, why, and what's next
2. **Human reviews**: Code diff, test results, build output
3. **Human decides**: Approve, request changes, or pivot direction
4. **Explicit proceed**: Human says "continue", "next", or provides new direction

### 4. Communication Protocol

**AI responsibilities:**
- Present changes concisely (summary + key files changed)
- Show test/build results
- Propose next step (wait for approval)
- Ask when uncertain, don't assume

**Human responsibilities:**
- Review diffs carefully
- Test functionality when needed
- Give clear go/no-go decisions
- Provide context for pivots

### 5. Development Workflow

#### Phase-based approach:
```
Phase 1: Setup → Review
Phase 2: Core Implementation → Review  
Phase 3: Integration → Review
Phase 4: Testing → Review
Phase 5: Documentation → Review
```

#### Per-feature workflow:
```
1. Design discussion (if complex)
2. Interface definition
3. Implementation
4. Unit test (if applicable)
5. Integration test
6. Documentation
7. Commit with descriptive message
```

### 6. Code Quality Standards

**Non-negotiables:**
- Compile with zero warnings
- Follow existing code style
- RAII principles for resource management
- Clear error handling
- Thread-safety annotations where needed

**Documentation:**
- Public APIs documented
- Complex logic explained
- Thread-safety guarantees stated
- MQTT message formats specified

### 7. Testing Strategy

**Build verification:**
- Full build after each change
- Docker container build if dependencies changed

**Runtime testing:**
- Mock-based unit tests for isolated components
- Integration tests for MQTT communication
- Hardware-in-loop when possible

### 8. Git Practices

**Commits:**
- Descriptive messages (what + why)
- Atomic commits (one logical change)
- Build passes before commit

**Branches:**
- `main`: stable, documented code
- `feature/*`: individual features
- `fix/*`: bug fixes

### 9. Decision Making

**AI decides:** Implementation details, code structure, naming
**Human decides:** Architecture, feature priorities, external integrations
**Both discuss:** Complex algorithms, threading models, API design

### 10. Context Preservation

**AI maintains:**
- Current phase/task
- Pending items list (in memory)
- Recent decisions made

**Human provides:**
- Hardware constraints
- Home Assistant integration specifics
- Performance requirements
- Deadline/priority shifts

## Quick Reference Commands

**Human shortcuts:**
- "continue" / "next" → Proceed with proposed next step
- "show diff" → Display changes made
- "revert" → Undo last change
- "explain" → Deep dive on implementation
- "pause" → Stop and summarize current state

## Success Metrics

- ✅ Each commit builds successfully in Docker
- ✅ No change breaks existing functionality
- ✅ Changes are understandable in 6 months
- ✅ MQTT integration testable without hardware
- ✅ Project publishable at any phase

---

**Current Phase:** Project Setup
**Next Milestone:** Docker environment + basic CMake structure
