# pg_slowjit

>  A simple demo to illustrate how to implement a JIT provider for PostgreSQL. `pg_slowjit` emits C codes and compile them to shared libraries in runtime. This project is inspired by [09 - Query Compilation & JIT Code Generation (CMU Advanced Databases / Spring 2023)](https://www.youtube.com/watch?v=eurwtUhY5fk).

## What does it support?

Currently, it only supports jitting a few operators. You can use it to jit the query `SELECT 1;`.

```
postgres=# EXPLAIN (SETTINGS ON) SELECT 1;
                                   QUERY PLAN
---------------------------------------------------------------------------------
 Result  (cost=0.00..0.01 rows=1 width=4)
 Settings: jit_above_cost = '0'
 JIT:
   Functions: 1
   Options: Inlining false, Optimization false, Expressions true, Deforming true
(5 rows)
```
