--Column ID of table A is primary key:

CREATE TABLE A (
	ID	int4 not null,
	id1	int4 not null,
primary key (ID,ID1)
);

--Columns REFB of table B and REFC of C are foreign keys referenting ID of A:

CREATE TABLE B (
	REFB	int4,
	REFB1	INT4
);
CREATE INDEX BI ON B (REFB);

CREATE TABLE C (
	REFC	int4,
	REFC1	int4
);
CREATE INDEX CI ON C (REFC);

--Trigger for table A:

CREATE TRIGGER AT BEFORE DELETE  ON A FOR EACH ROW
EXECUTE PROCEDURE 
check_foreign_key (2, 'cascade', 'ID','id1', 'B', 'REFB','REFB1', 'C', 'REFC','REFC1');


CREATE TRIGGER AT1  AFTER UPDATE  ON A FOR EACH ROW
EXECUTE PROCEDURE 
check_foreign_key (2, 'cascade', 'ID','id1', 'B', 'REFB','REFB1', 'C', 'REFC','REFC1');


CREATE TRIGGER BT BEFORE INSERT OR UPDATE ON B FOR EACH ROW
EXECUTE PROCEDURE 
check_primary_key ('REFB','REFB1', 'A', 'ID','ID1');

CREATE TRIGGER CT BEFORE INSERT OR UPDATE ON C FOR EACH ROW
EXECUTE PROCEDURE 
check_primary_key ('REFC','REFC1', 'A', 'ID','ID1');



-- Now try

INSERT INTO A VALUES (10,10);
INSERT INTO A VALUES (20,20);
INSERT INTO A VALUES (30,30);
INSERT INTO A VALUES (40,41);
INSERT INTO A VALUES (50,50);

INSERT INTO B VALUES (1);	-- invalid reference
INSERT INTO B VALUES (10,10);
INSERT INTO B VALUES (30,30);
INSERT INTO B VALUES (30,30);

INSERT INTO C VALUES (11);	-- invalid reference
INSERT INTO C VALUES (20,20);
INSERT INTO C VALUES (20,21);
INSERT INTO C VALUES (30,30);

-- now update work well 
update  A set ID = 100 , ID1 = 199 where ID=30 ;

SELECT * FROM A;
SELECT * FROM B;
SELECT * FROM C;
