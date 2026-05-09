module Sdram_Control(
		//	HOST Side
      REF_CLK,
      RESET_N,
		CLK,
		//	FIFO Write Side 
      WR_DATA,
		WR,
		WR_ADDR,
		WR_MAX_ADDR,
		WR_LENGTH,
		WR_LOAD,
		WR_CLK,
		WR_FULL,
		WR_USE,
		//	FIFO Read Side 
      RD_DATA,
		RD,
		RD_ADDR,
		RD_MAX_ADDR,
		RD_LENGTH,
		RD_LOAD,	
		RD_CLK,
		RD_EMPTY,
		RD_USE,
		//	SDRAM Side
        SA,
        BA,
        CS_N,
        CKE,
        RAS_N,
        CAS_N,
        WE_N,
        DQ,
        DQM,
		  SDR_CLK
        );


`include        "sdram_local_test/Sdram_Params.h"
//	HOST Side
input                           REF_CLK;                //System Clock
input                           RESET_N;                //System Reset
//	FIFO Write Side 1
input   [`DSIZE-1:0]            WR_DATA;               //Data input
input							        WR;					//Write Request
input	  [`ASIZE-1:0]			     WR_ADDR;				//Write start address
input	  [`ASIZE-1:0]			     WR_MAX_ADDR;			//Write max address
input	  [8:0]					     WR_LENGTH;				//Write length
input							        WR_LOAD;				//Write register load & fifo clear
input							        WR_CLK;				//Write fifo clock
output							     WR_FULL;				//Write fifo full
output  [15:0]					     WR_USE;				//Write fifo usedw

//	FIFO Read Side 1
output  [`DSIZE-1:0]            RD_DATA;               //Data output
input							        RD;					//Read Request
input	  [`ASIZE-1:0]			     RD_ADDR;				//Read start address
input	  [`ASIZE-1:0]			     RD_MAX_ADDR;			//Read max address
input	  [8:0]					     RD_LENGTH;				//Read length
input							        RD_LOAD;				//Read register load & fifo clear
input							        RD_CLK;				//Read fifo clock
output							     RD_EMPTY;				//Read fifo empty
output	[15:0]					  RD_USE;				//Read fifo usedw
//	SDRAM Side
output  [`SASIZE-1:0]           SA;                     //SDRAM address output
output  [1:0]                   BA;                     //SDRAM bank address
output  [1:0]                   CS_N;                   //SDRAM Chip Selects
output                          CKE;                    //SDRAM clock enable
output                          RAS_N;                  //SDRAM Row address Strobe
output                          CAS_N;                  //SDRAM Column address Strobe
output                          WE_N;                   //SDRAM write enable
inout   [`DSIZE-1:0]            DQ;                     //SDRAM data bus
output  [`DSIZE/8-1:0]          DQM;                    //SDRAM data mask lines
output							     SDR_CLK;				//SDRAM clock
//	Internal Registers/Wires
//	Controller
reg		[`ASIZE-1:0]			  mADDR;					//Internal address
reg		[8:0]					     mLENGTH;				//Internal length
wire	[`ASIZE-1:0]			  mLENGTH_ADDR;
reg		[`ASIZE-1:0]			  rWR_ADDR;				//Register write address
				
reg		[`ASIZE-1:0]			 rRD_ADDR;				//Register read address
reg							       WR_MASK;				//Write port active mask
reg							       RD_MASK;				//Read port active mask
reg								    mWR_DONE;				//Flag write done, 1 pulse SDR_CLK
reg								    mRD_DONE;				//Flag read done, 1 pulse SDR_CLK
reg								    mWR,Pre_WR;				//Internal WR edge capture
reg								    mRD,Pre_RD;				//Internal RD edge capture
reg 	[9:0] 					    ST;						//Controller status
reg		[1:0] 					 CMD;					//Controller command
reg								    PM_STOP;				//Flag page mode stop
reg								    PM_DONE;				//Flag page mode done
reg								    Read;					//Flag read active
reg								    Write;					//Flag write active
reg	    [`DSIZE-1:0]         mDATAOUT;               //Controller Data output

wire    [`DSIZE-1:0]           mDATAIN;                //Controller Data input 2
wire                           CMDACK;                 //Controller command acknowledgement
//	DRAM Control
reg  	[`DSIZE/8-1:0]            DQM;                    //SDRAM data mask lines
reg     [`SASIZE-1:0]           SA;                     //SDRAM address output
reg     [1:0]                   BA;                     //SDRAM bank address
reg     [1:0]                   CS_N;                   //SDRAM Chip Selects
reg                             CKE;                    //SDRAM clock enable
reg                             RAS_N;                  //SDRAM Row address Strobe
reg                             CAS_N;                  //SDRAM Column address Strobe
reg                             WE_N;                   //SDRAM write enable
wire    [`DSIZE-1:0]            DQOUT;					//SDRAM data out link
wire  	[`DSIZE/8-1:0]         IDQM;                   //SDRAM data mask lines
wire    [`SASIZE-1:0]           ISA;                    //SDRAM address output
wire    [1:0]                   IBA;                    //SDRAM bank address
wire    [1:0]                   ICS_N;                  //SDRAM Chip Selects
wire                            ICKE;                   //SDRAM clock enable
wire                            IRAS_N;                 //SDRAM Row address Strobe
wire                            ICAS_N;                 //SDRAM Column address Strobe
wire                            IWE_N;                  //SDRAM write enable
//	FIFO Control
reg								OUT_VALID;				//Output data request to read side fifo
reg								IN_REQ;					//Input	data request to write side fifo
wire	[15:0]					write_side_fifo_rusedw;
wire	[15:0]					read_side_fifo_wusedw;

//	DRAM Internal Control
wire    [`ASIZE-1:0]            saddr;
wire                            load_mode;
wire                            nop;
wire                            reada;
wire                            writea;
wire                            refresh;
wire                            precharge;
wire                            oe;
wire							ref_ack;
wire							ref_req;
wire							init_req;
wire							cm_ack;
wire							active;
wire							pll_locked;
reg		[1:0]					reset_sync = 2'b00;
wire							ctrl_reset_n;
output                  CLK;

// Host-side controls are generated on WR_CLK/RD_CLK and consumed on CLK.
// Load toggles hold address buses stable until the 100 MHz side acknowledges.
reg     [`ASIZE-1:0]            wr_addr_src;
reg     [`ASIZE-1:0]            wr_max_addr_src;
reg     [8:0]                   wr_length_src;
reg                             wr_length_en_src;
reg                             wr_load_src_d;
reg                             wr_load_toggle_src;
reg                             wr_load_pending_src;
(* altera_attribute = "-name SYNCHRONIZER_IDENTIFICATION FORCED" *) reg wr_load_ack_meta;
(* altera_attribute = "-name SYNCHRONIZER_IDENTIFICATION FORCED" *) reg wr_load_ack_sync;

reg     [`ASIZE-1:0]            rd_addr_src;
reg     [`ASIZE-1:0]            rd_max_addr_src;
reg     [8:0]                   rd_length_src;
reg                             rd_length_en_src;
reg                             rd_load_src_d;
reg                             rd_load_toggle_src;
reg                             rd_load_pending_src;
(* altera_attribute = "-name SYNCHRONIZER_IDENTIFICATION FORCED" *) reg rd_load_ack_meta;
(* altera_attribute = "-name SYNCHRONIZER_IDENTIFICATION FORCED" *) reg rd_load_ack_sync;

(* altera_attribute = "-name SYNCHRONIZER_IDENTIFICATION FORCED" *) reg wr_load_toggle_meta;
(* altera_attribute = "-name SYNCHRONIZER_IDENTIFICATION FORCED" *) reg wr_load_toggle_sync;
reg                             wr_load_toggle_seen;
reg                             wr_load_ack_toggle_clk;
reg                             wr_load_clk;
reg     [`ASIZE-1:0]            wr_addr_clk;
reg     [`ASIZE-1:0]            wr_max_addr_clk;
reg     [8:0]                   wr_length_meta;
reg     [8:0]                   wr_length_clk;
(* altera_attribute = "-name SYNCHRONIZER_IDENTIFICATION FORCED" *) reg wr_length_en_meta;
(* altera_attribute = "-name SYNCHRONIZER_IDENTIFICATION FORCED" *) reg wr_length_en_clk;

(* altera_attribute = "-name SYNCHRONIZER_IDENTIFICATION FORCED" *) reg rd_load_toggle_meta;
(* altera_attribute = "-name SYNCHRONIZER_IDENTIFICATION FORCED" *) reg rd_load_toggle_sync;
reg                             rd_load_toggle_seen;
reg                             rd_load_ack_toggle_clk;
reg                             rd_load_clk;
reg     [`ASIZE-1:0]            rd_addr_clk;
reg     [`ASIZE-1:0]            rd_max_addr_clk;
reg     [8:0]                   rd_length_meta;
reg     [8:0]                   rd_length_clk;
(* altera_attribute = "-name SYNCHRONIZER_IDENTIFICATION FORCED" *) reg rd_length_en_meta;
(* altera_attribute = "-name SYNCHRONIZER_IDENTIFICATION FORCED" *) reg rd_length_en_clk;

wire                            wr_load_src_rise;
wire                            rd_load_src_rise;
wire                            wr_load_ready_src;
wire                            rd_load_ready_src;
wire                            wr_fifo_aclr;
wire                            rd_fifo_aclr;
wire    [8:0]                   wr_length_ctrl;
wire    [8:0]                   rd_length_ctrl;

assign mLENGTH_ADDR = {{(`ASIZE-9){1'b0}}, mLENGTH};
assign ctrl_reset_n = reset_sync[1];
assign wr_load_src_rise = WR_LOAD & ~wr_load_src_d;
assign rd_load_src_rise = RD_LOAD & ~rd_load_src_d;
assign wr_load_ready_src = !wr_load_pending_src || (wr_load_ack_sync == wr_load_toggle_src);
assign rd_load_ready_src = !rd_load_pending_src || (rd_load_ack_sync == rd_load_toggle_src);
assign wr_fifo_aclr = !RESET_N || WR_LOAD;
assign rd_fifo_aclr = !RESET_N || RD_LOAD;
assign wr_length_ctrl = wr_length_en_clk ? wr_length_clk : 9'd0;
assign rd_length_ctrl = rd_length_en_clk ? rd_length_clk : 9'd0;

sdram_pll0 sdram_pll0_inst(
		.refclk(REF_CLK),   //  refclk.clk
		.rst(!RESET_N),      //   reset.reset
		.outclk_0(CLK), // outclk0.clk
		.outclk_1(SDR_CLK), // outclk1.clk
		.locked(pll_locked)    //  locked.export
		);

always@(posedge CLK or negedge RESET_N or negedge pll_locked)
begin
	if(!RESET_N || !pll_locked)
		reset_sync <= 2'b00;
	else
		reset_sync <= {reset_sync[0], 1'b1};
end

always@(posedge WR_CLK)
begin
	if(!RESET_N)
	begin
		wr_addr_src         <= {`ASIZE{1'b0}};
		wr_max_addr_src     <= {`ASIZE{1'b0}};
		wr_length_src       <= 9'd0;
		wr_length_en_src    <= 1'b0;
		wr_load_src_d       <= 1'b0;
		wr_load_toggle_src  <= 1'b0;
		wr_load_pending_src <= 1'b0;
		wr_load_ack_meta    <= 1'b0;
		wr_load_ack_sync    <= 1'b0;
	end
	else
	begin
		wr_load_src_d    <= WR_LOAD;
		wr_load_ack_meta <= wr_load_ack_toggle_clk;
		wr_load_ack_sync <= wr_load_ack_meta;
		wr_length_en_src <= (WR_LENGTH != 9'd0);
		if(WR_LENGTH != 9'd0)
			wr_length_src <= WR_LENGTH;

		if(wr_load_ready_src)
			wr_load_pending_src <= 1'b0;
		if(wr_load_src_rise && wr_load_ready_src)
		begin
			wr_addr_src         <= WR_ADDR;
			wr_max_addr_src     <= WR_MAX_ADDR;
			wr_load_toggle_src  <= ~wr_load_toggle_src;
			wr_load_pending_src <= 1'b1;
		end
	end
end

always@(posedge RD_CLK)
begin
	if(!RESET_N)
	begin
		rd_addr_src         <= {`ASIZE{1'b0}};
		rd_max_addr_src     <= {`ASIZE{1'b0}};
		rd_length_src       <= 9'd0;
		rd_length_en_src    <= 1'b0;
		rd_load_src_d       <= 1'b0;
		rd_load_toggle_src  <= 1'b0;
		rd_load_pending_src <= 1'b0;
		rd_load_ack_meta    <= 1'b0;
		rd_load_ack_sync    <= 1'b0;
	end
	else
	begin
		rd_load_src_d    <= RD_LOAD;
		rd_load_ack_meta <= rd_load_ack_toggle_clk;
		rd_load_ack_sync <= rd_load_ack_meta;
		rd_length_en_src <= (RD_LENGTH != 9'd0);
		if(RD_LENGTH != 9'd0)
			rd_length_src <= RD_LENGTH;

		if(rd_load_ready_src)
			rd_load_pending_src <= 1'b0;
		if(rd_load_src_rise && rd_load_ready_src)
		begin
			rd_addr_src         <= RD_ADDR;
			rd_max_addr_src     <= RD_MAX_ADDR;
			rd_load_toggle_src  <= ~rd_load_toggle_src;
			rd_load_pending_src <= 1'b1;
		end
	end
end

always@(posedge CLK)
begin
	if(!ctrl_reset_n)
	begin
		wr_load_toggle_meta    <= 1'b0;
		wr_load_toggle_sync    <= 1'b0;
		wr_load_toggle_seen    <= 1'b0;
		wr_load_ack_toggle_clk <= 1'b0;
		wr_load_clk            <= 1'b0;
		wr_addr_clk            <= {`ASIZE{1'b0}};
		wr_max_addr_clk        <= {`ASIZE{1'b0}};
		wr_length_meta         <= 9'd0;
		wr_length_clk          <= 9'd0;
		wr_length_en_meta      <= 1'b0;
		wr_length_en_clk       <= 1'b0;

		rd_load_toggle_meta    <= 1'b0;
		rd_load_toggle_sync    <= 1'b0;
		rd_load_toggle_seen    <= 1'b0;
		rd_load_ack_toggle_clk <= 1'b0;
		rd_load_clk            <= 1'b0;
		rd_addr_clk            <= {`ASIZE{1'b0}};
		rd_max_addr_clk        <= {`ASIZE{1'b0}};
		rd_length_meta         <= 9'd0;
		rd_length_clk          <= 9'd0;
		rd_length_en_meta      <= 1'b0;
		rd_length_en_clk       <= 1'b0;
	end
	else
	begin
		wr_load_clk         <= 1'b0;
		wr_load_toggle_meta <= wr_load_toggle_src;
		wr_load_toggle_sync <= wr_load_toggle_meta;
		wr_length_meta      <= wr_length_src;
		wr_length_clk       <= wr_length_meta;
		wr_length_en_meta   <= wr_length_en_src;
		wr_length_en_clk    <= wr_length_en_meta;
		if(wr_load_toggle_sync != wr_load_toggle_seen)
		begin
			wr_addr_clk            <= wr_addr_src;
			wr_max_addr_clk        <= wr_max_addr_src;
			wr_load_toggle_seen    <= wr_load_toggle_sync;
			wr_load_ack_toggle_clk <= wr_load_toggle_sync;
			wr_load_clk            <= 1'b1;
		end

		rd_load_clk         <= 1'b0;
		rd_load_toggle_meta <= rd_load_toggle_src;
		rd_load_toggle_sync <= rd_load_toggle_meta;
		rd_length_meta      <= rd_length_src;
		rd_length_clk       <= rd_length_meta;
		rd_length_en_meta   <= rd_length_en_src;
		rd_length_en_clk    <= rd_length_en_meta;
		if(rd_load_toggle_sync != rd_load_toggle_seen)
		begin
			rd_addr_clk            <= rd_addr_src;
			rd_max_addr_clk        <= rd_max_addr_src;
			rd_load_toggle_seen    <= rd_load_toggle_sync;
			rd_load_ack_toggle_clk <= rd_load_toggle_sync;
			rd_load_clk            <= 1'b1;
		end
	end
end

control_interface control1 (
                .CLK(CLK),
                .RESET_N(ctrl_reset_n),
                .CMD(CMD),
                .ADDR(mADDR),
                .REF_ACK(ref_ack),
                .CM_ACK(cm_ack),
                .NOP(nop),
                .READA(reada),
                .WRITEA(writea),
                .REFRESH(refresh),
                .PRECHARGE(precharge),
                .LOAD_MODE(load_mode),
                .SADDR(saddr),
                .REF_REQ(ref_req),
				    .INIT_REQ(init_req),
                .CMD_ACK(CMDACK)
                );

command command1(
                .CLK(CLK),
                .RESET_N(ctrl_reset_n),
                .SADDR(saddr),
                .NOP(nop),
                .READA(reada),
                .WRITEA(writea),
                .REFRESH(refresh),
				    .LOAD_MODE(load_mode),
                .PRECHARGE(precharge),
                .REF_REQ(ref_req),
				    .INIT_REQ(init_req),
                .REF_ACK(ref_ack),
                .CM_ACK(cm_ack),
                .OE(oe),
				    .PM_STOP(PM_STOP),
				    .PM_DONE(PM_DONE),
                .SA(ISA),
                .BA(IBA),
                .CS_N(ICS_N),
                .CKE(ICKE),
                .RAS_N(IRAS_N),
                .CAS_N(ICAS_N),
                .WE_N(IWE_N)
                );
                
sdr_data_path data_path1(
                .CLK(CLK),
                .RESET_N(ctrl_reset_n),
                .DATAIN(mDATAIN),
                .DM(2'b00),
                .DQOUT(DQOUT),
                .DQM(IDQM)
                );

Sdram_WR_FIFO 	write_fifo1(
				.data(WR_DATA),
				.wrreq(WR),
				.wrclk(WR_CLK),
				.aclr(wr_fifo_aclr),
				.rdreq(IN_REQ&WR_MASK),
				.rdclk(CLK),
				.q(mDATAIN),
				.wrfull(WR_FULL),
				.wrusedw(WR_USE),
				.rdusedw(write_side_fifo_rusedw)
				);
				
reg flag;
always@(posedge CLK)
begin
 if(!ctrl_reset_n)
 flag	<=	0;
 else
 begin
  if(write_side_fifo_rusedw==wr_length_ctrl)
  flag	<=	1;
 end
end				


Sdram_RD_FIFO 	read_fifo1(
				.data(mDATAOUT),
				.wrreq(OUT_VALID&RD_MASK),
				.wrclk(CLK),
				.aclr(rd_fifo_aclr),
				.rdreq(RD),
				.rdclk(RD_CLK),
				.q(RD_DATA),
				.wrusedw(read_side_fifo_wusedw),
				.rdempty(RD_EMPTY),
				.rdusedw(RD_USE)
				);
				

always @(posedge CLK)
begin
	SA      <= (ST==SC_CL+mLENGTH)			?	13'h200	:	ISA;
    BA      <= IBA;
    CS_N    <= ICS_N;
    CKE     <= ICKE;
    RAS_N   <= (ST==SC_CL+mLENGTH)			?	1'b0	:	IRAS_N;
    CAS_N   <= (ST==SC_CL+mLENGTH)			?	1'b1	:	ICAS_N;
    WE_N    <= (ST==SC_CL+mLENGTH)			?	1'b0	:	IWE_N;
	PM_STOP	<= (ST==SC_CL+mLENGTH)			?	1'b1	:	1'b0;
	PM_DONE	<= (ST==SC_CL+SC_RCD+mLENGTH+2)	?	1'b1	:	1'b0;
	DQM		<= ( active && (ST>=SC_CL) )	?	(	((ST==SC_CL+mLENGTH) && Write)?	2'b11	:	2'b00	)	:	2'b11	;
	mDATAOUT<= DQ;
end

assign  DQ = oe ? DQOUT : `DSIZE'hzzzz;
assign	active	=	Read | Write;

always@(posedge CLK)
begin
	if(ctrl_reset_n==0)
	begin
		CMD			<=  0;
		ST			<=  0;
		Pre_RD		<=  0;
		Pre_WR		<=  0;
		Read		<=	0;
		Write		<=	0;
		OUT_VALID	<=	0;
		IN_REQ		<=	0;
		mWR_DONE	<=	0;
		mRD_DONE	<=	0;
	end
	else
	begin
		Pre_RD	<=	mRD;
		Pre_WR	<=	mWR;
		case(ST)
		0:	begin
				if({Pre_RD,mRD}==2'b01)
				begin
					Read	<=	1;
					Write	<=	0;
					CMD		<=	2'b01;
					ST		<=	1;
				end
				else if({Pre_WR,mWR}==2'b01)
				begin
					Read	<=	0;
					Write	<=	1;
					CMD		<=	2'b10;
					ST		<=	1;
				end
			end
		1:	begin
				if(CMDACK==1)
				begin
					CMD<=2'b00;
					ST<=2;
				end
			end
		default:	
			begin	
				if(ST!=SC_CL+SC_RCD+mLENGTH+1)
				ST<=ST+1;
				else
				ST<=0;
			end
		endcase
	
		if(Read)
		begin
			if(ST==SC_CL+SC_RCD+1)
			OUT_VALID	<=	1;
			else if(ST==SC_CL+SC_RCD+mLENGTH+1)
			begin
				OUT_VALID	<=	0;
				Read		<=	0;
				mRD_DONE	<=	1;
			end
		end
		else
		mRD_DONE	<=	0;
		
		if(Write)
		begin
			if(ST==SC_CL-1)
			IN_REQ	<=	1;
			else if(ST==SC_CL+mLENGTH-1)
			IN_REQ	<=	0;
			else if(ST==SC_CL+SC_RCD+mLENGTH)
			begin
				Write	<=	0;
				mWR_DONE<=	1;
			end
		end
		else
		mWR_DONE<=	0;

	end
end
//	Internal Address & Length Control
always@(posedge CLK)
begin
	if(!ctrl_reset_n)
	begin
		rWR_ADDR		<=	{`ASIZE{1'b0}};
		rRD_ADDR		<=	{`ASIZE{1'b0}};
	end
	else
	begin
		//	Write Side
		if(wr_load_clk)
			rWR_ADDR	<=	wr_addr_clk;
		else if(mWR_DONE&WR_MASK)
		begin
			// WR_LENGTH can be throttled to zero between bursts by the client.
			// Use the latched burst length that actually completed.
			if(rWR_ADDR<wr_max_addr_clk-mLENGTH_ADDR)
				rWR_ADDR	<=	rWR_ADDR+mLENGTH_ADDR;
			else
				rWR_ADDR	<=	wr_addr_clk;
		end

		//	Read Side
		if(rd_load_clk)
			rRD_ADDR	<=	rd_addr_clk;
		else if(mRD_DONE&RD_MASK)
		begin
			// RD_LENGTH may also be flow-controlled while a burst is active.
			if(rRD_ADDR<rd_max_addr_clk-mLENGTH_ADDR)
				rRD_ADDR	<=	rRD_ADDR+mLENGTH_ADDR;
			else
				rRD_ADDR	<=	rd_addr_clk;
		end
	end
end
//	Auto Read/Write Control
always@(posedge CLK)
begin
	if(!ctrl_reset_n)
	begin
		mWR		<=	0;
		mRD		<=	0;
		mADDR	<=	0;
		mLENGTH	<=	0;
		WR_MASK <=	0;
		RD_MASK <=	0;
	end
	else
	begin
		if( (mWR==0) && (mRD==0) && (ST==0) &&
			(WR_MASK==0)	&&	(RD_MASK==0) &&
			(wr_load_clk==0)	&&	(rd_load_clk==0) &&(flag==1) )
		begin
			//	Read Side: scanout needs bounded latency; let
			//	pending reads win over queued write bursts.
			if( (read_side_fifo_wusedw < rd_length_ctrl) && (rd_length_ctrl!=0) )
			begin
				mADDR	<=	rRD_ADDR;
				mLENGTH	<=	rd_length_ctrl;
				WR_MASK	<=	1'b0;
				RD_MASK	<=	1'b1;
				mWR		<=	0;
				mRD		<=	1;
			end
			//	Write Side
			else if( (write_side_fifo_rusedw >= wr_length_ctrl) && (wr_length_ctrl!=0) )
			begin
				mADDR	<=	rWR_ADDR;
				mLENGTH	<=	wr_length_ctrl;
				WR_MASK	<=	1'b1;
				RD_MASK	<=	1'b0;
				mWR		<=	1;
				mRD		<=	0;
			end
		end
		if(mWR_DONE)
		begin
			WR_MASK	<=	0;
			mWR		<=	0;
		end
		if(mRD_DONE)
		begin
			RD_MASK	<=	0;
			mRD		<=	0;
		end
	end
end

endmodule
