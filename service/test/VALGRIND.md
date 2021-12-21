It is recommended, when debugging, to run these tests with valgrind:
```
valgrind --track-origins=yes --read-inline-info=yes --leak-check=full --show-leak-kinds=all -v --trace-children=yes --read-var-info=yes <test>
```
