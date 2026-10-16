#import "/tools/common.typ": *

= Tasks <tasks>

#part("Syntax")
```ebnf
tasks-decl = "tasks" NAME size [ size [ size ] ] ;   (* count, call depth, region bytes *)
```

#para("1")[A *task* is a flow of execution that takes turns with others inside one state. Tasks change only where one
gives way on its own (`yield`, or a host word that waits). So even when two tasks use the same global, no other task
comes in between two yields of one task. Tasks are not threads: a state runs on one thread at a time (5.1).]

#para("2")[`tasks NAME count [depth [region]]` declares a #term("task group") of up to `count` tasks. Each task has its
own call depth (`depth`) and its own region (`region` bytes); left out, they are the processor's defaults. The name of
the group is the type of the values that refer to its tasks.]

== Making and running tasks <tasks-run>

#para("1")[`block GROUP spawn` makes one task from a block value (10.5) that takes and gives nothing, and gives the task
(`or fail`, `err.full` when there is no free slot). A new task is stopped and has not run yet.]

#para("2")[`GROUP run_all` runs the tasks of the group in turn until all have ended. A task runs until it yields or
ends; then the next one goes on.]

#ex("spec", "14-tasks.pnx")

#para("3")[`TASK resume` runs that one task up to its next yield or its end, and gives `true` when it has ended.
`TASK is_done` asks whether it has ended. The slot of an ended task is used again by `spawn`.]

#ex("spec", "14-resume.pnx")

#para("4")[Tasks stay in the state. When a run the host started ends, a stopped task is still there and can go on in
the next run.]

#ex("spec", "14-full.pnx")

== Yielding <tasks-yield>

#para("1")[`yield` passes the turn to another task. It can fail (`or fail`): when the task was cancelled it fails with
`err.cancelled`. In the main run, which is not a task, `yield` does nothing and succeeds at once.]

#para("2")[Tasks share data through globals, queues and lists. A full or empty queue is a reason to yield.]

#ex("spec", "14-queue.pnx")

#para("3")[Which words may yield (directly or in a word they call) is known at load. `wait_take` and `wait_put` (14.3)
yield too. Such words cannot be run from inside a host word (clause 19).]

== Waiting for a queue <tasks-queue-wait>

#para("1")[`QUEUE wait_take` takes the front value of a queue as `take` does, but while the queue is empty it yields and
looks again when its turn comes back. `v QUEUE wait_put` puts like `put`, yielding while the queue is full. Both can
fail (`or fail`): a task cancelled while it waits fails with `err.cancelled`. When the value can be taken or put at
once, cancelling is not looked at.]

#ex("spec", "14-queue-wait.pnx")

#para("2")[A task that waits for a queue does not run again until some queue has changed. When `run_all` finds that
every task left waits for a queue and none waits for the host, nothing can change a queue any more: that is the fault
`deadlock`. Like a fault inside a task, it ends every task. While a task waits for the host, the state waits
(`PINAX_WAITING`) as before; what that task does when it goes on can free the others.]

#ex("spec", "14-deadlock.pnx")

#para("3")[The main run is not a task and has no one to give its turn to. In the main run, `wait_take` on an empty
queue and `wait_put` on a full one are the fault `deadlock` at once.]

#ex("spec", "14-deadlock-main.pnx")

#para("4")[Only these two words are watched for a deadlock. Tasks that wait in a loop of their own (`loop … yield …
end`), or a task resumed by hand with `resume`, are not; a host that runs such code gives it a budget (19.5).]

== Cancelling <tasks-cancel>

#para("1")[`TASK cancel` tells a task it is cancelled: its next `yield`, or its wait for a queue (14.3), fails with
`err.cancelled`. The task takes that
failure and ends in its own way. Cancelling does not stop a task by force.]

#ex("spec", "14-cancel.pnx")

#part("Constraints")

#para("2")[The block given to `spawn` is a block value and does not see the locals around it. Values reach a task
through globals and queues.]
#ex("spec", "14-spawn-refused.pnx")

#part("Faults")

#para("3")[A fault inside a task ends the whole run and every task. The fault goes to the host like a fault of the main
run. The state remains and takes the next run.]
#ex("spec", "14-fault.pnx")

#para("4")[Two tasks using the same region are not followed by the check of 12.3. Each task has a region of its own, so
this does not normally happen.]
