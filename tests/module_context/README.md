# ModuleContext integration test

Tests that calls to ModuleContext.RemoveSelfFromStory() are handled correctly by the framework.

The user shell starts a story and adds two modules to it. It then signals the
first module to call ModuleContext.RemoveSelfFromStory and verifies that the module is stopped.
The user shell then signals the second module, and verifies that the story is
stopped as well.


